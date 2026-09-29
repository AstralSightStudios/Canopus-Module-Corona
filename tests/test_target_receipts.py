"""Target-binding regressions using synthetic ELF containers and ephemeral keys.

These exercise the delivery verifier, not the Canopus ELF loader or production
signer trust. The synthetic fixtures must never be used as installable payloads.
"""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verify_payload', ROOT / 'scripts/verify-payload.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


def fixture_elf(target):
    elf = bytearray(512)
    elf[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHI', elf, 16, 1, 40, 1)
    struct.pack_into('<I', elf, 32, 64)
    struct.pack_into('<HHHHHH', elf, 40, 52, 0, 0, 40, 3, 1)
    names = b'\0.shstrtab\0.rh.target\0'
    struct.pack_into('<10I', elf, 104, 1, 3, 0, 0, 256, len(names), 0, 0, 1, 0)
    struct.pack_into('<10I', elf, 144, 11, 1, 0, 0, 320, 48, 0, 0, 1, 0)
    elf[256:256 + len(names)] = names
    elf[320:368] = target.encode().ljust(48, b'\0')
    return bytes(elf)


class TargetReceipts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='resource-hook-target-test-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.private = self.directory / 'ephemeral-test-private.pem'
        self.public = self.directory / 'ephemeral-test-public.pem'
        self.run_command('openssl', 'genpkey', '-algorithm', 'ED25519', '-out', self.private)
        self.run_command('openssl', 'pkey', '-in', self.private, '-pubout', '-out', self.public)

    @staticmethod
    def run_command(*command):
        return subprocess.run([str(arg) for arg in command], check=True, capture_output=True)

    def payload(self, target, *, artifact_target=None, firmware=None):
        elf = fixture_elf(artifact_target or target)
        (self.directory / 'resource-hook.elf').write_bytes(elf)
        message = (struct.pack('<8I', int.from_bytes(b'CMI1', 'little'), 1, 256,
                               0, 1, verifier.MODULE_VERSION, len(elf), 0)
                   + verifier.MODULE_ID.encode().ljust(32, b'\0')
                   + target.encode().ljust(48, b'\0')
                   + bytes.fromhex(firmware or verifier.TARGETS[target])
                   + hashlib.sha256(elf).digest() + b'canopus-release\0')
        source, signature = self.directory / 'message', self.directory / 'signature'
        source.write_bytes(message)
        self.run_command('openssl', 'pkeyutl', '-sign', '-inkey', self.private,
                         '-rawin', '-in', source, '-out', signature)
        (self.directory / 'receipt.bin').write_bytes(message + signature.read_bytes())

    def test_each_target_requires_matching_receipt_and_artifact(self):
        for target in verifier.TARGETS:
            with self.subTest(target=target):
                self.payload(target)
                self.assertEqual(len(verifier.verify(self.directory, self.public, target)), 64)
                other = next(t for t in verifier.TARGETS if t != target)
                with self.assertRaisesRegex(ValueError, 'target ID'):
                    verifier.verify(self.directory, self.public, other)

    def test_resigning_wrong_target_artifact_is_not_a_port(self):
        for target in verifier.TARGETS:
            other = next(t for t in verifier.TARGETS if t != target)
            with self.subTest(target=target):
                self.payload(target, artifact_target=other)
                with self.assertRaisesRegex(ValueError, 'artifact target'):
                    verifier.verify(self.directory, self.public, target)

    def test_wrong_firmware_fingerprint_rejected_even_with_valid_signature(self):
        target = 'xiaomi-band-11-4.100.155'
        self.payload(target, firmware=verifier.TARGETS[verifier.TARGET])
        with self.assertRaisesRegex(ValueError, 'firmware fingerprint'):
            verifier.verify(self.directory, self.public, target)

    def test_default_target_does_not_infer_155_from_receipt(self):
        self.payload('xiaomi-band-11-4.100.155')
        with self.assertRaisesRegex(ValueError, 'target ID'):
            verifier.verify(self.directory, self.public)

    def test_unknown_target_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unsupported target'):
            verifier.verify(self.directory, self.public, 'xiaomi-band-11-4.100.108')

    def test_cli_requires_explicit_155_target(self):
        target = 'xiaomi-band-11-4.100.155'
        self.payload(target)
        command = [sys.executable, str(ROOT / 'scripts/verify-payload.py'),
                   str(self.directory), '--public-key', str(self.public)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('target ID', result.stderr)
        result = subprocess.run(command + ['--target', target], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(target, result.stdout)

    def test_cli_requires_explicit_1043_target(self):
        target = 'xiaomi-band-10-pro-3.101.043'
        self.payload(target)
        command = [sys.executable, str(ROOT / 'scripts/verify-payload.py'),
                   str(self.directory), '--public-key', str(self.public)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('target ID', result.stderr)
        result = subprocess.run(command + ['--target', target], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(target, result.stdout)

    def test_malformed_section_table_rejected_without_crashing(self):
        elf = bytearray(fixture_elf(verifier.TARGET))
        for offset, value in ((32, 0xffffffff), (144, 0xffffffff), (160, 0xffffffff)):
            with self.subTest(offset=offset):
                malformed = bytearray(elf)
                struct.pack_into('<I', malformed, offset, value)
                with self.assertRaises(ValueError):
                    verifier.verify_artifact_target(malformed, verifier.TARGET)


if __name__ == '__main__':
    unittest.main(verbosity=2)
