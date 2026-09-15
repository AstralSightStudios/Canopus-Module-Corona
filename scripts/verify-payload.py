#!/usr/bin/env python3
"""Verify the fixed-target CMI1 delivery against an independently trusted key."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

TARGET = "xiaomi-band-11-4.100.139"
FIRMWARE = "31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74"
MODULE_ID = "resource_hook"
MODULE_VERSION = 3
MAX_ARTIFACT = 393216


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(directory, public_key):
    elf = (directory / "resource-hook.elf").read_bytes()
    receipt = (directory / "receipt.bin").read_bytes()
    require(512 <= len(elf) <= MAX_ARTIFACT, "ELF size outside Supervisor limits")
    require(elf[:7] == b"\x7fELF\x01\x01\x01", "expected little-endian ELF32")
    require(struct.unpack_from("<HH", elf, 16) == (1, 40), "expected ARM relocatable ELF")
    require(len(receipt) == 256, "CMI1 receipt must be exactly 256 bytes")
    require(struct.unpack_from("<8I", receipt) ==
            (int.from_bytes(b"CMI1", "little"), 1, 256, 0, 1, MODULE_VERSION, len(elf), 0),
            "CMI1 header, lifecycle, version or size mismatch")
    require(receipt[32:64] == MODULE_ID.encode().ljust(32, b"\0"), "module ID mismatch")
    require(receipt[64:112] == TARGET.encode().ljust(48, b"\0"), "target ID mismatch")
    require(receipt[112:144] == bytes.fromhex(FIRMWARE), "firmware fingerprint mismatch")
    require(receipt[144:176] == hashlib.sha256(elf).digest(), "artifact digest mismatch")
    require(receipt[176:192] == b"canopus-release\0", "unexpected signing key ID")
    with tempfile.TemporaryDirectory(prefix="resource-hook-verify-") as tmp:
        message, signature = Path(tmp) / "message", Path(tmp) / "signature"
        message.write_bytes(receipt[:192])
        signature.write_bytes(receipt[192:])
        result = subprocess.run([
            "openssl", "pkeyutl", "-verify", "-pubin", "-inkey", str(public_key),
            "-rawin", "-in", str(message), "-sigfile", str(signature),
        ], capture_output=True, text=True)
        require(result.returncode == 0, "Ed25519 receipt signature does not match trusted key")
    return hashlib.sha256(elf).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--public-key", type=Path, required=True,
                        help="trusted Supervisor signer public PEM; do not trust an unverified bundled key")
    args = parser.parse_args()
    try:
        digest = verify(args.directory, args.public_key)
    except (OSError, ValueError) as error:
        parser.exit(1, f"payload rejected: {error}\n")
    print(f"payload verified: {MODULE_ID} / {TARGET} / version {MODULE_VERSION} / SHA256 {digest}")


if __name__ == "__main__":
    main()
