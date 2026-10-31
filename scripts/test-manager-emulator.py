#!/usr/bin/env python3
"""Corona native manager acceptance on real Mirage firmware (.043 / .155 / p65).

One target per invocation; run two processes with separate --library and --work.
Consumes prebuilt production installer resources and an immutable .canopus package.
No dependency builds or sibling checkout writes. Exit 77 means missing tooling or
firmware. Screenshots alone produce `captured`, never `passed` or device evidence.

Example (set MIRAGECTL, MAB_SMOKE and FACETORY_PACKAGER for your machine):
  python3 scripts/test-manager-emulator.py 155 --library /tmp/corona-155-library \
    --work /tmp/corona-155-evidence --package dist/canopus_corona-0.3.0.canopus \
    --firmware-source /path/to/miwear.watch.q66tc_v4.100.155_full_f1c824fe \
    --scenario tests/fixtures/manager-emulator/155.json

--seed-dir mirrors /data/canopus/m.corona and is copied while the device is off.
A scenario is JSON {"steps": [...], "probe": {"path": "ui-probe.txt",
"required": ["regex"], "forbidden": ["renderfail"]}}. Supported steps:
  {"tap": [x,y], "settle": 0.6}, {"swipe": [x0,y0,x1,y1], "duration": 900},
  {"wait": 1}, {"shot": "home", "pixels": [{"point": [10,90],
     "rgb": [29,163,98], "tolerance": 3}]},
  {"shot": "back", "same_as": "home", "crop": [0,70,212,500]}.
Transfer injection belongs to an explicitly compiled UI test hook: a scenario
may contain its touch sequence and seed input, but never sends Connect DATA.
"""
import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import socket
import struct
import subprocess
import sys
import time
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARKER = ".corona-manager-emulator"


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def executable(env, candidates):
    if os.environ.get(env):
        return Path(os.environ[env]).expanduser().resolve()
    return next((Path(p) for p in candidates if Path(p).is_file()), Path(candidates[0]))


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", choices=("043", "155", "p65"))
    parser.add_argument("--canopus-root", type=Path, default=Path(os.environ.get("CANOPUS_ROOT", ROOT.parent / "Canopus-Private")))
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--installer-dir", type=Path)
    parser.add_argument("--firmware-source", type=Path)
    parser.add_argument("--seed-dir", type=Path)
    parser.add_argument("--seed-resources", action="store_true",
                        help="manually place package resources into m.corona (fallback); default lets the install auto-extract them")
    parser.add_argument("--scenario", type=Path)
    parser.add_argument("--keep-device", action="store_true", help="preserve the dedicated emulator device after shutdown")
    parser.add_argument("--preflight", action="store_true", help="check tools, firmware and inputs without creating a device")
    args = parser.parse_args()
    args.canopus_root = args.canopus_root.resolve()
    args.library = args.library.resolve()
    args.work = args.work.resolve()
    args.package = args.package.resolve()
    default_library = (Path.home() / "Library/Application Support/Mirage").resolve()
    if args.library == default_library or default_library in args.library.parents:
        parser.error("--library must be separate from the daily Mirage library")
    if args.library.exists() and any(args.library.iterdir()) and not (args.library / MARKER).is_file():
        parser.error("existing nonempty --library lacks the dedicated Corona test marker")
    if args.work == args.library or args.library in args.work.parents or args.work in args.library.parents:
        parser.error("--work and --library must be separate directories")
    if args.work.exists() and any(args.work.iterdir()):
        parser.error("--work must be empty so stale evidence cannot pass assertions")
    if args.installer_dir is None:
        family = {"043": "xiaomi-band-10-pro", "155": "xiaomi-band-11",
                  "p65": "redmi-watch"}[args.target]
        args.installer_dir = args.canopus_root / "watchfaces/canopus-installer-prod" / family
    return parser, args


def configure(args):
    home = Path.home()
    mirage = home / "develop/Mirage"
    os.environ["MIRAGECTL"] = str(executable("MIRAGECTL", [
        "/Volumes/EXT0/Mirage/.build/out/Products/Debug/miragectl",
        "/tmp/canopus-mirage-build/out/Products/Debug/miragectl",
        mirage / "Packages/MirageKit/.build/out/Products/Debug/miragectl"]))
    os.environ["MAB_SMOKE"] = str(executable("MAB_SMOKE", [
        "/Volumes/EXT0/Mirage/AstroBoxBridge/target/smoke", mirage / "AstroBoxBridge/target/smoke"]))
    os.environ["FACETORY_PACKAGER"] = str(executable("FACETORY_PACKAGER", [
        "/Volumes/EXT0/GitHub/facetory/rust/target/debug/examples/package_lua_watchface",
        home / "develop/flutter/Facetory/rust/target/debug/examples/package_lua_watchface",
        home / "develop/flutter/Facetory/rust/target/release/examples/package_lua_watchface",
        home / "develop/Flutter/Facetory/rust/target/debug/examples/package_lua_watchface",
        home / "develop/Flutter/Facetory/rust/target/release/examples/package_lua_watchface"]))
    os.environ["MIRAGE_QEMU"] = str(executable("MIRAGE_QEMU", [
        mirage / "External/qemu-miwear/build/qemu-system-arm",
        "/Volumes/EXT0/Mirage/External/qemu-miwear/build/qemu-system-arm"]))
    os.environ["MIRAGE_LIBRARY"] = str(args.library)
    sys.path.insert(0, str(args.canopus_root / "scripts/emu"))
    ui = importlib.import_module("ui_pages")
    native = importlib.import_module("native_ui_v2")
    class RedmiWatch6(ui.Band10Pro):
        """P65 panel and wide native content; touch positions await Mirage validation.

        The installer uses a wrapping row: 196+196px title/status fit side by
        side, followed by two 184px buttons. Its Run centre is (124, 380).
        Native content begins at y=56 and uses 96px rows with 8px gaps.
        """
        name = "p65"
        target = "redmi-watch-6-3.100.043"
        firmware = "miwear.watch.p65_v3.100.043_full_662aa7db"
        size = (432, 514)
        wake_taps = [(216, 257), (216, 257)]
        pair_wake = [(216, 257), (216, 257)]
        pair_confirm = (330, 468)
        run = (124, 380)
        idle = (216, 490)
        dot = None
        tutorial_skip = None
        launcher_open = (216, 490, 216, 70)
        launcher_scroll = (16, (216, 470, 216, 70))
        manager_icon = (360, 288)
        icon_probe = (360, 248)
        home_scroll = (4, (216, 450, 216, 120))
        modules_row = (216, 190)
        first_module = (216, 145)
        scroll_top = (2, (216, 150, 216, 470))
        third_row = (216, 340)
        dialog_confirm = (216, 459)
        level2_back = (30, 28)
        close_page = ("tap", (30, 28))
        reboot_before_pages = False

    ui.P = {"043": ui.Band10Pro, "155": ui.Band11, "p65": RedmiWatch6}[args.target]
    ui.P.device = f"corona-manager-{args.target}-{time.time_ns()}"
    ui.M = {"id": "canopus_corona"}
    return ui, native


def check_inputs(args, ui):
    missing = [str(p) for p in (ui.MIRAGECTL, ui.BRIDGE, ui.PACKAGER, Path(os.environ["MIRAGE_QEMU"])) if not p.is_file() or not os.access(p, os.X_OK)]
    if not args.package.is_file():
        missing.append(str(args.package))
    supervisor = args.installer_dir / f"canopus_supervisor-{ui.P.target}.bin"
    required_installer = [args.installer_dir / "main.lua", supervisor]
    if args.target == "p65":
        required_installer.extend(args.installer_dir / f"canopus_{part}-{ui.P.target}.bin"
                                  for part in ("stage1", "stage2", "loader_profile"))
    for path in required_installer:
        if not path.is_file():
            missing.append(str(path))
    if missing:
        print("SKIP missing prerequisites: " + ", ".join(missing))
        return False
    # .canopus is the SDK's tar container, not a ZIP or the old RPK layout.
    with tarfile.open(args.package, "r:*") as package:
        member = package.getmember("manifest.json")
        source = package.extractfile(member)
        if source is None:
            raise ValueError("module package lacks manifest.json")
        with source:
            manifest = json.load(source)
        if manifest.get("module_id") != "canopus_corona" or manifest.get("canopus_abi") != "2":
            raise ValueError("expected Corona native ABI 2 package")
        artifact = next((a for a in manifest.get("artifacts", []) if a.get("target_id") == ui.P.target), None)
        if artifact is None:
            raise ValueError(f"package does not contain {ui.P.target}")
        for name in (artifact["path"], artifact["receipt_path"]):
            if not package.getmember(name).isfile():
                raise ValueError(f"package artifact is not a file: {name}")
    if args.seed_dir and not args.seed_dir.is_dir():
        raise ValueError("--seed-dir does not exist")
    return True


def load_scenario(path):
    if not path:
        return {"steps": [{"shot": "corona-home"}]}
    scenario = json.loads(path.read_text())
    if not isinstance(scenario.get("steps"), list) or not scenario["steps"]:
        raise ValueError("scenario requires nonempty steps")
    probe = scenario.get("probe")
    if probe:
        relative_path(probe["path"])
        if not probe.get("required"):
            raise ValueError("probe requires positive assertions")
    return scenario


def relative_path(path):
    value = PurePosixPath(path)
    if value.is_absolute() or not value.parts or any(part in (".", "..") for part in value.parts) or "\\" in path:
        raise ValueError("fixture / probe path must remain below m.corona")
    return value


def place_package_resources(args, ui, package_copy):
    """Mirror the installer's documented resource extraction into the module
    data root. `.canopus` carries images under `resources/common/` and
    `resources/<target_id>/`; install auto-extracts each to the module data
    directory as one flat file (Canopus-Private scripts/emu/module_smoke.py).
    The current AstroBox bridge stages only ELF+receipt and drops `resources/`,
    so the harness delivers them exactly as that extraction would, letting the
    module's resource resolution be validated independently of the sender."""
    base = PurePosixPath("/canopus/m.corona")
    prefixes = ("resources/common/", f"resources/{ui.P.target}/")
    ui.ctl("fs", "mkdir", ui.P.device, "nand_data", "/canopus")
    ui.ctl("fs", "mkdir", ui.P.device, "nand_data", str(base))
    placed = 0
    with tarfile.open(package_copy, "r:*") as package:
        for member in package.getmembers():
            name = member.name
            if not member.isfile() or not name.startswith(prefixes):
                continue
            filename = name.rsplit("/", 1)[-1]
            source = package.extractfile(member)
            if source is None:
                raise ValueError(f"package resource is not readable: {name}")
            with source, tempfile.NamedTemporaryFile(delete=False) as tmp:
                tmp.write(source.read())
                tmp_path = tmp.name
            try:
                ui.ctl("fs", "put", ui.P.device, "nand_data", tmp_path, str(base / filename))
            finally:
                os.unlink(tmp_path)
            placed += 1
    if placed == 0:
        raise ValueError("package declared no common/target resources to extract")
    return placed


def seed(args, ui):
    """Never modify a live LittleFS volume (Band 11 has no NSH)."""
    if not args.seed_dir:
        return
    base = args.seed_dir.resolve()
    paths = sorted(base.rglob("*"))
    for path in paths:
        if path.is_symlink():
            raise ValueError("seed tree must not contain symlinks")
    directories = {PurePosixPath("/canopus"), PurePosixPath("/canopus/m.corona")}
    for path in paths:
        rel = relative_path(path.relative_to(base).as_posix())
        dest = PurePosixPath("/canopus/m.corona") / rel
        directories.add(dest if path.is_dir() else dest.parent)
    for directory in sorted(directories, key=lambda p: (len(p.parts), str(p))):
        ui.ctl("fs", "mkdir", ui.P.device, "nand_data", directory)
    for path in paths:
        if path.is_file():
            ui.ctl("fs", "put", ui.P.device, "nand_data", path,
                   PurePosixPath("/canopus/m.corona") / path.relative_to(base).as_posix())


def image_region(ui, path, crop):
    rows, bpp = ui.png_rows(path)
    if crop is None:
        crop = [0, 70, ui.P.size[0], ui.P.size[1]]
    x0, y0, x1, y1 = crop
    if not (0 <= x0 < x1 <= ui.P.size[0] and 0 <= y0 < y1 <= len(rows)):
        raise ValueError("invalid screenshot comparison crop")
    return b"".join(bytes(row[x0 * bpp:x1 * bpp]) for row in rows[y0:y1])


def held_drag(ui, gesture, hold=0.8, duration=0.6, repeat=1):
    """Hold before moving using MirageKit's local RFB pointer protocol.

    Mirage's linear swipe moves immediately and cannot reliably long-press.
    This connects only to the dedicated device's Unix socket.
    """
    if len(gesture) != 4 or repeat not in (1, 2) or not (0 <= hold <= 10 and 0 < duration <= 10):
        raise ValueError("drag needs four coordinates and bounded hold/duration")
    info = ui.ctl("info", ui.P.device)
    did = next(line.split()[-1] for line in info.splitlines() if line.startswith("id:"))
    endpoint = Path(tempfile.gettempdir()) / ("mirage-" + did.split("-")[0].lower()) / "vnc.sock"
    x0, y0, x1, y1 = gesture
    for x, y in ((x0, y0), (x1, y1)):
        if not (0 <= x < ui.P.size[0] and 0 <= y < ui.P.size[1]):
            raise ValueError("drag leaves target panel")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(10)
        client.connect(str(endpoint))

        def read(count):
            data = bytearray()
            while len(data) < count:
                part = client.recv(count - len(data))
                if not part:
                    raise RuntimeError("Mirage VNC connection closed")
                data.extend(part)
            return bytes(data)

        if not read(12).startswith(b"RFB 003."):
            raise RuntimeError("Mirage socket is not an RFB server")
        client.sendall(b"RFB 003.008\n")
        security = read(1)[0]
        if not security or 1 not in read(security):
            raise RuntimeError("Mirage VNC requires authentication")
        client.sendall(b"\x01")
        if read(4) != b"\0\0\0\0":
            raise RuntimeError("Mirage VNC security handshake failed")
        client.sendall(b"\x01")
        initial = read(24)
        read(struct.unpack_from(">I", initial, 20)[0])
        if struct.unpack_from(">HH", initial) != ui.P.size:
            raise RuntimeError("Mirage VNC panel dimensions differ from target")
        for press in range(repeat):
            try:
                client.sendall(struct.pack(">BBHH", 5, 1, x0, y0))
                time.sleep(hold)
                for step in range(1, 13):
                    x = round(x0 + (x1 - x0) * step / 12)
                    y = round(y0 + (y1 - y0) * step / 12)
                    client.sendall(struct.pack(">BBHH", 5, 1, x, y))
                    time.sleep(duration / 12)
            finally:
                client.sendall(struct.pack(">BBHH", 5, 0, x1, y1))
            if press + 1 < repeat:
                time.sleep(0.12)


def scenario_steps(ui, scenario, shots, record):
    images = {}
    assertions = 0
    for step in scenario["steps"]:
        operations = set(step) & {"tap", "double_tap", "swipe", "drag", "wait", "shot"}
        if len(operations) != 1:
            raise ValueError("scenario step needs exactly one operation")
        if "tap" in step:
            ui.tap(tuple(step["tap"]), step.get("settle", 0.6))
        elif "double_tap" in step:
            x, y = step["double_tap"]
            held_drag(ui, [x, y, x, y], 0.08, 0.01, repeat=2)
            time.sleep(step.get("settle", 0.6))
        elif "swipe" in step:
            gesture = step["swipe"]
            if len(gesture) != 4:
                raise ValueError("swipe needs four coordinates")
            ui.ctl("swipe", ui.P.device, *gesture, step.get("duration", 300))
            time.sleep(step.get("settle", 0.6))
        elif "drag" in step:
            held_drag(ui, step["drag"], float(step.get("hold", 0.8)), float(step.get("duration", 0.6)))
            time.sleep(step.get("settle", 0.6))
        elif "wait" in step:
            seconds = float(step["wait"])
            if not 0 <= seconds <= 60:
                raise ValueError("wait must be 0..60 seconds")
            time.sleep(seconds)
        else:
            name = step["shot"]
            if not name or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in name):
                raise ValueError("screenshot names may contain only letters, digits, - and _")
            path = shots.take(name, lit=False)
            rows, bpp = ui.png_rows(path)
            if len(rows) != ui.P.size[1] or len(rows[0]) != ui.P.size[0] * bpp:
                raise RuntimeError("screenshot dimensions do not match the target")
            for assertion in step.get("pixels", []):
                observed = ui.pixel(path, *assertion["point"])
                expected = assertion["rgb"]
                tolerance = assertion.get("tolerance", 0)
                if len(expected) != 3 or any(abs(a - b) > tolerance for a, b in zip(observed, expected)):
                    raise RuntimeError(f"{name}: pixel {assertion['point']} expected {expected}, got {observed}")
                assertions += 1
            if "same_as" in step:
                old = images[step["same_as"]]
                if image_region(ui, path, step.get("crop")) != image_region(ui, old, step.get("crop")):
                    raise RuntimeError(f"{name}: body differs from {step['same_as']}")
                assertions += 1
            images[name] = path
        record.setdefault("completed_steps", []).append(step)
    return assertions


def check_probe(ui, work, scenario):
    import re
    probe = scenario.get("probe")
    if not probe:
        return 0
    path = PurePosixPath("/canopus/m.corona") / relative_path(probe["path"])
    output = work / "ui-probe.txt"
    ui.ctl("fs", "get", ui.P.device, "nand_data", path, output)
    data = output.read_text()
    for pattern in probe["required"]:
        if re.search(pattern, data, re.MULTILINE) is None:
            raise RuntimeError(f"UI probe missing {pattern!r}; see {output}")
    for pattern in probe.get("forbidden", [r"^renderfail\b"]):
        if re.search(pattern, data, re.MULTILINE):
            raise RuntimeError(f"UI probe contains {pattern!r}; see {output}")
    return len(probe["required"])


def main():
    parser, args = arguments()
    if not (args.canopus_root / "scripts/emu/native_ui_v2.py").is_file():
        print("SKIP missing Canopus emulator helpers")
        return 77
    ui, native = configure(args)
    if not check_inputs(args, ui):
        return 77
    scenario = load_scenario(args.scenario)
    args.library.mkdir(parents=True, exist_ok=True)
    (args.library / MARKER).write_text("Dedicated Corona manager emulator acceptance library\n")
    firmware = ui.firmware_id()
    if firmware is None and args.firmware_source:
        ui.ctl("firmware", "import", args.firmware_source.resolve())
        firmware = ui.firmware_id()
    if firmware is None:
        print(f"SKIP .{args.target} firmware unavailable; pass --firmware-source")
        return 77
    print("Tools:", json.dumps({"MIRAGECTL": str(ui.MIRAGECTL), "MAB_SMOKE": str(ui.BRIDGE),
                                 "FACETORY_PACKAGER": str(ui.PACKAGER)}))
    if args.preflight:
        print("Preflight passed:", ui.P.target, firmware)
        return 0
    args.work.mkdir(parents=True, exist_ok=True)
    shots = ui.Shots(args.work / "screenshots")
    package_copy = args.work / "corona-input.canopus"
    shutil.copyfile(args.package, package_copy)
    record = {"level": "host/emulator", "target": ui.P.target, "device": ui.P.device,
              "library": str(args.library), "firmware": firmware, "module": "canopus_corona",
              "source_package": str(args.package), "installed_package": str(package_copy),
              "module_package_sha256": digest(package_copy), "bridge_sha256": digest(ui.BRIDGE),
              "qemu_sha256": digest(Path(os.environ["MIRAGE_QEMU"])),
              "tools": {name: os.environ[name] for name in ("MIRAGECTL", "MAB_SMOKE", "FACETORY_PACKAGER", "MIRAGE_QEMU")},
              "status": "running",
              "visual_review": "not performed by harness", "scenario": scenario,
              **({"touch_coordinates": "implementation-derived; pending emulator validation"}
                 if args.target == "p65" else {}),
              "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    (args.work / "run.json").write_text(json.dumps(record, indent=2) + "\n")
    created = False
    try:
        installer = native.installer(args.installer_dir, args.work)
        record["supervisor_sha256"] = digest(args.work / "face" / f"canopus_supervisor-{ui.P.target}.bin")
        ui.ctl("create", ui.P.device, "--firmware", firmware)
        created = True
        seed(args, ui)
        if args.seed_resources:
            record["resources_extracted"] = place_package_resources(args, ui, package_copy)
            record["resources_via"] = "harness-seeded"
        else:
            # Rely on the real install auto-extracting resources/ into the
            # module data dir (Canopus 26/27/28 -> /canopus/m.<id>/<name>).
            record["resources_via"] = "install-autoextract"
        ui.prepare(args.work, installer, package_copy, shots)
        ui.open_module_page(shots)
        count = scenario_steps(ui, scenario, shots, record)
        ui.ctl("shutdown", ui.P.device)
        count += check_probe(ui, args.work, scenario)
        record["assertions_passed"] = count
        record["status"] = "passed" if count else "captured"
        print("Evidence:", args.work, "status:", record["status"])
        return 0
    except Exception as error:
        record["status"] = "failed"
        record["error"] = str(error)
        if created:
            try:
                shots.take("failure", lit=False)
            except Exception as screenshot_error:
                record["failure_screenshot_error"] = str(screenshot_error)
        raise
    finally:
        if created:
            ui.ctl("shutdown", ui.P.device, check=False)
            if not args.keep_device:
                ui.ctl("delete", ui.P.device, check=False)
        record["screenshots"] = {p.name: digest(p) for p in sorted(shots.directory.glob("*.png"))}
        record["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        (args.work / "run.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    sys.stdout.reconfigure(line_buffering=True)
    sys.exit(main())
