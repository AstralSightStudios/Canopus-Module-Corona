#!/usr/bin/env python3
"""Reproducible host experiments, with a separate parameterized link cost model."""
from __future__ import annotations

import argparse
import hashlib
import heapq
from collections import deque
import io
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import struct
import subprocess
import tempfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("CANOPUS_ROOT", str(ROOT.parent / "Canopus-Private"))).resolve()
PROFILES = [
    # Each adjacent A step changes one factor. B steps compare checkpoint/limits.
    dict(name="A0-original", header=32, json=1, group=1, buffer=0, checkpoint=65536, verify=1),
    dict(name="A1-header12", header=12, json=1, group=1, buffer=0, checkpoint=65536, verify=1),
    dict(name="A2-binary-ack", header=12, json=0, group=1, buffer=0, checkpoint=65536, verify=1),
    dict(name="A3-query4", header=12, json=0, group=4, buffer=0, checkpoint=65536, verify=1),
    dict(name="A4-query8", header=12, json=0, group=8, buffer=0, checkpoint=65536, verify=1),
    dict(name="A5-buffer4k", header=12, json=0, group=8, buffer=1, checkpoint=65536, verify=1),
    dict(name="B1-checkpoint256k", header=12, json=0, group=8, buffer=1, checkpoint=262144, verify=1),
    dict(name="B2-query16", header=12, json=0, group=16, buffer=1, checkpoint=262144, verify=1),
    dict(name="B3-no-periodic-checkpoint", header=12, json=0, group=8, buffer=1, checkpoint=0, verify=1),
    dict(name="B4-no-disk-sha", header=12, json=0, group=8, buffer=1, checkpoint=262144, verify=0),
    dict(name="C0-push-ack4", header=12, json=0, group=4, buffer=1, checkpoint=65536, verify=1, push=1),
]


def run_json(args: list[str]) -> dict:
    return json.loads(subprocess.check_output(args, text=True))


def build(directory: Path, sanitize: bool = False) -> Path:
    cc = os.environ.get("CC", "cc")
    sha = directory / ("sha-san.o" if sanitize else "sha.o")
    binary = directory / ("receiver-san" if sanitize else "receiver")
    includes = ["-I" + str(SDK / "sdk/c"), "-I" + str(SDK / "third_party/sha256")]
    # Existing SHA has signed-shift idioms; exercise it under ASan, with UBSan on our prototype.
    subprocess.run([cc, "-std=c11", "-O2", *includes,
                    *(["-fsanitize=address", "-fno-omit-frame-pointer"] if sanitize else []),
                    "-c", str(SDK / "third_party/sha256/sha256.c"), "-o", str(sha)], check=True)
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *includes,
                    *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else []),
                    str(ROOT / "tests/protocol/canopusconnect_bench.c"), str(sha), "-lz", "-o", str(binary)], check=True)
    return binary


def fixture(count: int, size: int, entropy: bool) -> bytes:
    rng = random.Random(20261023)
    manifest = {"format": "canopus-resource-pack", "formatVersion": 1,
                "themeId": "ablation", "name": "Ablation",
                "mappings": [{"source": "/resource/icons/", "destination": "icons/"}]}
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        files = [("corona.json", json.dumps(manifest, separators=(",", ":")).encode())]
        for i in range(count):
            data = rng.randbytes(size) if entropy else ((b"CORONA-RESOURCE-" + bytes([i % 256])) * (size // 16 + 1))[:size]
            files.append((f"icons/{i:04d}.bin", data))
        for name, data in files:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=6)
        archive.comment = b"CORONA-ABLATION-FIXTURE"
    return stream.getvalue()


def link_model(row: dict, profile: dict, rpc_ms: float = 20, event_ms: float = 10,
               wire_kib_s: float = 128, write_ms: float = 0.2, sync_ms: float = 5) -> dict:
    """Additive serialized service-cost model, NOT a discrete-event BLE simulation."""
    grouped = profile["group"] > 1
    queries = row["acks"] if grouped and not profile.get("push") else 0
    # Connect outer/CPC2/WearPacket/crypto overhead is deliberately not guessed.
    app_wire = row["data_wire_bytes"] + row["reply_wire_bytes"] + queries * profile["header"]
    elapsed_ms = ((row["frames"] + queries) * rpc_ms + row["acks"] * event_ms
                  + app_wire / (wire_kib_s * 1024) * 1000
                  + (row["payload_writes"] + row["checkpoint_writes"]) * write_ms
                  + row["syncs"] * sync_ms)
    return dict(seconds=elapsed_ms / 1000,
                kib_per_s=row["bytes"] / 1024 / (elapsed_ms / 1000),
                query_rpcs=queries, event_frames=row["acks"], app_wire_bytes=app_wire,
                assumptions=dict(rpc_ms=rpc_ms, event_ms=event_ms, app_wire_kib_s=wire_kib_s,
                                 write_ms=write_ms, sync_ms=sync_ms))


def recovery_sweep(size: int, header: int, group: int, cp_bytes: int,
                   rpc_ms: float, event_ms: float, timed: bool = False) -> dict:
    """Exact prefix/checkpoint accounting for this deterministic cost schedule."""
    chunk = 480 - header
    written = durable = max_replay = sync_points = 0
    last_sync_ms = clock = 0.0
    weighted_replay = 0.0
    while written < size:
        length = min(chunk * group, size - written)
        frames = math.ceil(length / chunk)
        duration = frames * rpc_ms + rpc_ms + event_ms
        # Reboot between group boundaries: bytes in this batch must also be resent.
        max_replay = max(max_replay, written + length - durable)
        weighted_replay += ((written - durable) + length / 2) * duration
        written += length
        clock += duration
        if (cp_bytes and written - durable >= cp_bytes) or (timed and clock - last_sync_ms >= 1000):
            durable = written
            last_sync_ms = clock
            sync_points += 1
    if durable < size:
        sync_points += 1
    return dict(max_replay_bytes=max_replay,
                mean_replay_bytes_at_uniform_time=weighted_replay / clock,
                checkpoint_count=sync_points,
                time_model="serialized RPC/group boundaries; excludes I/O time")


def sliding_model(size: int, rpc_window: int, app_window: int = 8, ack_every: int = 8,
                  rtt_ms: float = 20, wire_kib_s: float = 128,
                  owner_tick_ms: float = 10, bt_busy_ms: float = 2, bt_idle_ms: float = 10) -> dict:
    """Discrete-event hypothetical pipeline, ordered writer, FIFO owner, priority RPC TX.

    Includes finite RX (4), response TX (4), event TX (2), no radio retransmit/loss.
    RPC reply modeled as 32 bytes; real framing/crypto and firmware execution are not run.
    """
    queue: list[tuple] = []
    serial = 0
    rpc_tx, event_tx = deque(), deque()
    pending_ack = None
    draining = False
    sent = acked = in_flight = arrived = processed = rx_live = 0
    rx_peak = rpc_peak = event_peak = overflow = rpc_count = ack_count = 0
    cpu_free = uplink_free = last_time = 0.0
    tx_ms_byte = 1000 / (wire_kib_s * 1024)

    def schedule(time: float, kind: str, value=None) -> None:
        nonlocal serial
        serial += 1
        heapq.heappush(queue, (time, serial, kind, value))

    def send_more(time: float) -> None:
        nonlocal sent, in_flight, uplink_free, rpc_count
        limit = min(size, acked + app_window * 468)
        while in_flight < rpc_window and sent < limit:
            length = min(468, size - sent, limit - sent)
            end = sent + length
            start = max(time, uplink_free)
            uplink_free = start + (length + 12) * tx_ms_byte
            schedule(uplink_free + rtt_ms / 2, "arrive", (end, length))
            sent = end
            in_flight += 1
            rpc_count += 1

    def drain_later(time: float) -> None:
        nonlocal draining
        if not draining:
            draining = True
            schedule(math.ceil((time + 1e-9) / bt_idle_ms) * bt_idle_ms, "drain")

    send_more(0)
    while queue:
        time, _, kind, value = heapq.heappop(queue)
        last_time = time
        if kind == "arrive":
            arrived += 1
            rx_live += 1
            rx_peak = max(rx_peak, rx_live)
            if rx_live > 4:
                overflow += 1  # Mark invalid; do not pretend overflow is a successful run.
            end, length = value
            start = math.ceil(max(time, cpu_free) / owner_tick_ms) * owner_tick_ms
            cpu_free = start + owner_tick_ms
            boundary = arrived % ack_every == 0 or end == size
            schedule(start + (0.2 if boundary else 0.02), "processed", (end, boundary))
        elif kind == "processed":
            rx_live -= 1
            processed += 1
            end, boundary = value
            rpc_tx.append(("rpc_done", None, 32))
            rpc_peak = max(rpc_peak, len(rpc_tx))
            if len(rpc_tx) > 4:
                overflow += 1
            if boundary:
                if len(event_tx) < 2:
                    event_tx.append(("ack", end, 20))
                else:
                    pending_ack = end  # Module retains newest cumulative ACK until room exists.
                event_peak = max(event_peak, len(event_tx))
            drain_later(time)
        elif kind == "drain":
            draining = False
            if rpc_tx or event_tx:
                out_kind, out_value, length = (rpc_tx if rpc_tx else event_tx).popleft()
                schedule(time + length * tx_ms_byte + rtt_ms / 2, out_kind, out_value)
                if pending_ack is not None and len(event_tx) < 2:
                    event_tx.append(("ack", pending_ack, 20))
                    pending_ack = None
                if rpc_tx or event_tx:
                    draining = True
                    schedule(time + max(bt_busy_ms, length * tx_ms_byte), "drain")
        elif kind == "rpc_done":
            in_flight -= 1
            send_more(time)
        elif kind == "ack":
            acked = max(acked, value)
            ack_count += 1
            send_more(time)
        else:
            raise AssertionError(kind)
    assert acked == size and in_flight == 0
    return dict(rpc_window=rpc_window, app_window=app_window, ack_every=ack_every,
                seconds=last_time / 1000, kib_per_s=size / 1024 / (last_time / 1000),
                rx_peak=rx_peak, rpc_tx_peak=rpc_peak, event_tx_peak=event_peak,
                capacity_violations=overflow, valid=overflow == 0, data_rpcs=rpc_count,
                ack_events=ack_count, assumptions=dict(rtt_ms=rtt_ms, wire_kib_s=wire_kib_s,
                owner_tick_ms=owner_tick_ms, bt_busy_ms=bt_busy_ms, bt_idle_ms=bt_idle_ms,
                reply_bytes=32, flush_ms=0.2, parse_ms=0.02),
                scope="hypothetical concurrent core; ignores fsync, loss, outer TX serialization and exact framing")


def corruption_check(data: bytes) -> dict:
    corrupted = bytearray(data)
    corrupted[-1] ^= 1  # ZIP comment: entries and their CRCs are unchanged.
    with zipfile.ZipFile(io.BytesIO(corrupted)) as archive:
        crc_accepts = archive.testzip() is None
    sha_rejects = hashlib.sha256(data).digest() != hashlib.sha256(corrupted).digest()
    assert crc_accepts and sha_rejects
    return dict(zip_crc_only_accepts_modified_archive=crc_accepts, disk_sha_rejects=sha_rejects,
                mutation="one byte in ZIP comment, not an asset; CRC alone is not archive identity")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/experiments/canopusconnect-ablation.json")
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    assert args.repeats > 0
    fixtures = {"entropy-1m": fixture(64, 16384, True),
                "compressible-1m": fixture(64, 16384, False),
                "many-files-1m": fixture(2048, 512, True)}
    result = dict(schema=1, host=dict(system=platform.system(), machine=platform.machine(),
                  python=platform.python_version(), compiler=subprocess.check_output([os.environ.get("CC", "cc"), "--version"], text=True).splitlines()[0]),
                  profiles=PROFILES, fixtures={}, measurements={}, limitations=[
                      "C receiver hot-path prototype, not production Corona/Canopus or a complete control parser",
                      "Memory is sizeof explicitly owned C objects, not RSS, ARM stack, firmware heap, or baseline-document allocation",
                      "Timing is host POSIX/page-cache/fsync timing; no wearable throughput measured",
                      "Link numbers are a parameterized additive cost model; no exact WearPacket overhead or radio scheduling model",
                      "Codec memory uses host system zlib with tracked allocator; not a chosen embedded inflate library",
                      "No power-cut/fsync atomicity/flash wear/real UI scheduling proof",
                      "Data path, offset checks and fault injection run in C; JSON ACK is snprintf, control-message parsing is not benchmarked",
                      "A0 uses the original 64 KiB checkpoint threshold without its 1 second timer; timer policy is compared separately in recovery",
                      "Fixture .bin bytes are synthetic transport data, not device-validated LVGL assets"
                  ])
    with tempfile.TemporaryDirectory(prefix="corona-ablation-") as temp:
        directory = Path(temp)
        binary = build(directory)
        sanitized = build(directory, sanitize=True)
        result["fault_injection"] = run_json([str(sanitized), "--selftest"])
        result["measurements"] = {}
        for name, data in fixtures.items():
            path = directory / (name + ".crpack")
            path.write_bytes(data)
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                unpacked = sum(i.file_size for i in archive.infolist())
                files = len(archive.infolist())
            result["fixtures"][name] = dict(archive_bytes=len(data), unpacked_bytes=unpacked, files=files,
                                          sha256=hashlib.sha256(data).hexdigest())
            rows = {}
            for profile in PROFILES:
                command = [str(binary), str(path), *[str(profile[k]) for k in ("header", "json", "group", "buffer", "checkpoint", "verify")]]
                trials = [run_json(command) for _ in range(args.repeats)]
                row = dict(trials[0])
                for key in ("host_write_ms", "host_verify_ms"):
                    row[key] = statistics.median(t[key] for t in trials)
                row["trials"] = trials
                row["link_model"] = link_model(row, profile)
                rows[profile["name"]] = row
            result["measurements"][name] = rows
        result["corruption_ablation"] = corruption_check(fixtures["entropy-1m"])
        # Real raw-DEFLATE decoding, constant I/O buffers, measured native allocator peak.
        result["inflate"] = []
        for size in (65536, 1048576, 8388608):
            payload = (b"CORONA-RESOURCE-" * ((size + 14) // 15))[:size]
            compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
            compressed = compressor.compress(payload) + compressor.flush()
            path = directory / f"inflate-{size}.raw"
            path.write_bytes(compressed)
            row = run_json([str(binary), "--inflate", str(path)])
            assert row["stream_end"] and row["output_bytes"] == size
            row["whole_archive_output_buffer_bytes"] = size
            result["inflate"].append(row)
        # A valid large-distance stream cannot be made low-memory by shrinking history.
        prefix = random.Random(271828).randbytes(30000)
        payload = prefix * 4
        compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
        path = directory / "long-distance.raw"
        path.write_bytes(compressor.compress(payload) + compressor.flush())
        full = run_json([str(binary), "--inflate", str(path)])
        small = run_json([str(binary), "--inflate-small", str(path)])
        assert full["stream_end"] and full["output_bytes"] == len(payload)
        assert not small["stream_end"]
        result["history_window_ablation"] = dict(full_32k=full, small_8k=small,
            conclusion="8 KiB history rejects a valid original Deflate stream; do not advertise full compatibility")
        base = result["measurements"]["entropy-1m"]["A0-original"]
        chosen = result["measurements"]["entropy-1m"]["B1-checkpoint256k"]
        chosen_profile = next(p for p in PROFILES if p["name"] == "B1-checkpoint256k")
        result["sensitivity"] = []
        for rpc_ms, event_ms in ((5, 2), (20, 10), (80, 40), (80, 2)):
            a = link_model(base, PROFILES[0], rpc_ms, event_ms)
            b = link_model(chosen, chosen_profile, rpc_ms, event_ms)
            result["sensitivity"].append(dict(rpc_ms=rpc_ms, event_ms=event_ms,
                baseline_kib_s=a["kib_per_s"], candidate_kib_s=b["kib_per_s"],
                improvement_percent=(b["kib_per_s"] / a["kib_per_s"] - 1) * 100))
        result["recovery"] = {}
        for cp in (65536, 262144, 0):
            result["recovery"][str(cp)] = recovery_sweep(8 * 1024 * 1024, 12, 8, cp, 20, 10)
        result["recovery"]["64k-or-1s-original-policy"] = recovery_sweep(8 * 1024 * 1024, 12, 8, 65536, 20, 10, timed=True)
        result["sliding_window"] = []
        for rpc_window in (1, 2, 3, 4, 8):
            for app_window, ack_every in ((8, 8), (8, 4), (16, 8)):
                result["sliding_window"].append(sliding_model(len(fixtures["entropy-1m"]), rpc_window, app_window, ack_every))
        result["sliding_sensitivity"] = []
        for rtt_ms, wire_kib_s in ((5, 128), (20, 128), (80, 128), (20, 1024)):
            for rpc_window in (1, 2, 3, 4, 8):
                result["sliding_sensitivity"].append(sliding_model(len(fixtures["entropy-1m"]), rpc_window,
                    rtt_ms=rtt_ms, wire_kib_s=wire_kib_s))
        result["owner_tick_ablation"] = [sliding_model(len(fixtures["entropy-1m"]), 3, ack_every=4,
            owner_tick_ms=tick) for tick in (10, 5, 2)]
        # Deterministic Bernoulli loss at group progress event: duplicate QUERY, no DATA resend.
        rng = random.Random(314159)
        losses = sum(rng.random() < 0.01 for _ in range(chosen["acks"]))
        result["event_loss_model"] = dict(seed=314159, probability=0.01, groups=chosen["acks"],
            lost_events=losses, recovery_query_rpcs=losses, data_bytes_retransmitted=0,
            timeout_seconds=8, added_wait_seconds=losses * 8,
            scope="one possible event loss per group; second QUERY always delivered; C selftest covers idempotent QUERY")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Wrote {args.output}")
    print("profile                 frames   events   writes  syncs   model KiB/s")
    for name, row in result["measurements"]["entropy-1m"].items():
        print(f"{name:24} {row['frames']:6} {row['acks']:8} {row['payload_writes']:8} {row['syncs']:6} {row['link_model']['kib_per_s']:13.2f}")
    print("Fault checks:", result["fault_injection"])
    print("Inflate peaks:", [(r["output_bytes"], r["codec_heap_peak"] + r["io_buffers"] + r["zstream_bytes"]) for r in result["inflate"]])


if __name__ == "__main__":
    main()
