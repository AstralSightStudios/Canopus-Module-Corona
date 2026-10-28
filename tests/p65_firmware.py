"""Exact P65 AP loading for static and instruction-level Resource Hook tests."""
import hashlib
import os
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = "redmi-watch-6-3.100.043"
AP_SHA256 = "a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858"
OTA = ROOT.parent.parent / "mifw/miwear.watch.p65_v3.100.043_full_662aa7db.bin"
XIP = 0x0C0C0000
CACHED = 0x2C0C0000


def firmware():
    override = os.environ.get("RH_AP_P65")
    if os.environ.get("RESOURCE_HOOK_TARGET", TARGET) == TARGET:
        override = override or os.environ.get("RESOURCE_HOOK_FIRMWARE")
    if override:
        data = Path(override).read_bytes()
    elif OTA.is_file():
        with zipfile.ZipFile(OTA) as archive:
            names = [name for name in archive.namelist() if Path(name).name == "vela_ap.bin"]
            if len(names) != 1:
                raise ValueError("P65 OTA must contain one vela_ap.bin")
            data = archive.read(names[0])
    else:
        raise FileNotFoundError("Set RH_AP_P65 to the exact P65 AP or supply " + str(OTA))
    digest = hashlib.sha256(data).hexdigest()
    if digest != AP_SHA256:
        raise ValueError("P65 firmware fingerprint mismatch: " + digest)
    return data


def word(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def macros():
    import re
    import subprocess
    text = subprocess.check_output([os.environ.get("CLANG", "clang"), "-E", "-dM",
        "-DRH_TARGET_P65=1", "-I" + str(ROOT / "include"), "-x", "c",
        str(ROOT / "include/resource_hook_target.h")], text=True)
    return {key: int(value.rstrip("uUlL"), 0) for key, value in re.findall(
        r"^#define (RH_\w+) ((?:0x[0-9a-fA-F]+|[0-9]+)[uUlL]*)$", text, re.M)}
