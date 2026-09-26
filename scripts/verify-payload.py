#!/usr/bin/env python3
"""Verify a pinned-target CMI1 delivery against an independently trusted key."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

TARGET = "xiaomi-band-11-4.100.139"
TARGETS = {
    TARGET: "31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74",
    "xiaomi-band-11-4.100.155": "ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f",
}
FIRMWARE = TARGETS[TARGET]  # Backward-compatible .139 default.
MODULE_ID = "corona"
MODULE_VERSION = 3
MAX_ARTIFACT = 393216


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify_artifact_target(elf, target):
    # ELF32 section table, with bounded offsets and a unique PROGBITS identity.
    # This section is emitted from RH_TARGET_ID alongside the descriptor.
    section_offset = struct.unpack_from("<I", elf, 32)[0]
    section_size, count, names_index = struct.unpack_from("<HHH", elf, 46)
    require(section_size == 40 and 0 < names_index < count and
            52 <= section_offset <= len(elf) - count * section_size,
            "invalid ELF section table")
    sections = [struct.unpack_from("<10I", elf, section_offset + i * section_size)
                for i in range(count)]

    def contents(section):
        offset, size = section[4:6]
        require(offset <= len(elf) and size <= len(elf) - offset,
                "ELF section outside artifact")
        return elf[offset:offset + size]

    require(sections[names_index][1] == 3, "invalid ELF section names")
    names = contents(sections[names_index])
    identities = []
    for section in sections:
        require(section[0] < len(names), "invalid ELF section name")
        end = names.find(b"\0", section[0])
        require(end >= 0, "unterminated ELF section name")
        if names[section[0]:end] == b".rh.target":
            require(section[1] == 1, "invalid artifact target section")
            identities.append(contents(section))
    require(identities == [target.encode().ljust(48, b"\0")],
            "artifact target ID mismatch or missing identity")


def verify(directory, public_key, target=TARGET):
    require(target in TARGETS, "unsupported target")
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
    require(receipt[64:112] == target.encode().ljust(48, b"\0"), "target ID mismatch")
    require(receipt[112:144] == bytes.fromhex(TARGETS[target]), "firmware fingerprint mismatch")
    require(receipt[144:176] == hashlib.sha256(elf).digest(), "artifact digest mismatch")
    require(receipt[176:192] == b"canopus-release\0", "unexpected signing key ID")
    verify_artifact_target(elf, target)
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
    parser.add_argument("--target", choices=TARGETS, default=TARGET,
                        help="expected target (default: %(default)s); never inferred from the receipt")
    parser.add_argument("--public-key", type=Path, required=True,
                        help="trusted Supervisor signer public PEM; do not trust an unverified bundled key")
    args = parser.parse_args()
    try:
        digest = verify(args.directory, args.public_key, args.target)
    except (OSError, ValueError) as error:
        parser.exit(1, f"payload rejected: {error}\n")
    print(f"payload verified: {MODULE_ID} / {args.target} / version {MODULE_VERSION} / SHA256 {digest}")


if __name__ == "__main__":
    main()
