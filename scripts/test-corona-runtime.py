#!/usr/bin/env python3
"""Build and exercise the complete device transfer path through SDK adapters."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("CANOPUS_ROOT", ROOT.parent / "Canopus-Private"))
BINARY = ROOT / "build/test_corona_runtime"
BINARY.parent.mkdir(exist_ok=True)
subprocess.run([
    os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
    "-fsanitize=address,undefined", "-g", f"-I{ROOT / 'include'}", f"-I{SDK / 'sdk/c'}",
    str(ROOT / "src/corona_connect.c"), str(ROOT / "src/corona_archive.c"),
    str(ROOT / "src/corona_runtime.c"), str(ROOT / "src/manager_data.c"),
    str(ROOT / "tests/test_corona_runtime.c"), "-o", str(BINARY),
], check=True)
manifest = {
    "format": "canopus-resource-pack", "formatVersion": 1,
    "themeId": "integration", "name": "Integration theme", "version": "1.0",
    "mappings": [{"source": "/resource/integration.bin", "destination": "images/icon.bin"}],
}
with tempfile.TemporaryDirectory(prefix="corona-runtime-") as work:
    work = Path(work)
    archive = work / "input.crpack"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as package:
        package.writestr("corona.json", json.dumps(manifest))
        package.writestr("images/icon.bin", bytes(range(256)) * 32)
    # A directory listed out of local order defeats streaming; the runtime must
    # discard that staging and install through full extraction instead.
    data = archive.read_bytes()
    end = data.rfind(b"PK\x05\x06")
    cd = struct.unpack_from("<I", data, end + 16)[0]
    records, at = [], cd
    while data[at:at+4] == b"PK\x01\x02":
        n = 46 + sum(struct.unpack_from("<HHH", data, at + 28))
        records.append(data[at:at+n])
        at += n
    reordered = work / "reordered.crpack"
    reordered.write_bytes(data[:cd] + b"".join(reversed(records)) + data[end:])
    for mode in ("normal", "recovery", "cancel", "legacy", "rmdir-failure", "fallback"):
        package = reordered if mode == "fallback" else archive
        sandbox = work / mode
        sandbox.mkdir()
        abandoned = sandbox / "staging/abandoned/nested"
        abandoned.mkdir(parents=True)
        (abandoned / "old.bin").write_bytes(b"abandoned upload")
        subprocess.run([
            str(BINARY), str(sandbox), str(package),
            hashlib.sha256(package.read_bytes()).hexdigest(), mode,
        ], check=True)
