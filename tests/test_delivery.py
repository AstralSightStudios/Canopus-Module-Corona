"""Delivery checks require a built payload; failures are not skipped."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
TARGET = os.environ.get('RESOURCE_HOOK_TARGET', 'xiaomi-band-11-4.100.139')
PAYLOAD = Path(os.environ.get('RESOURCE_HOOK_PAYLOAD',
    ROOT / 'build/payload-0.3.0' / TARGET))
spec = importlib.util.spec_from_file_location('verify_payload', ROOT / 'scripts/verify-payload.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class Delivery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='resource-hook-delivery-test-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        for name in ('receipt.bin', 'resource-hook.elf', 'signer-public.pem'):
            shutil.copyfile(PAYLOAD / name, self.directory / name)
        self.key = self.directory / 'signer-public.pem'

    def check(self):
        return verifier.verify(self.directory, self.key, TARGET)

    def test_valid_receipt(self):
        self.assertEqual(len(self.check()), 64)

    def test_wrong_expected_target(self):
        other = next(target for target in verifier.TARGETS if target != TARGET)
        with self.assertRaisesRegex(ValueError, 'target ID'):
            verifier.verify(self.directory, self.key, other)

    def test_tampered_artifact(self):
        p = self.directory / 'resource-hook.elf'
        b = bytearray(p.read_bytes()); b[-1] ^= 1; p.write_bytes(b)
        with self.assertRaisesRegex(ValueError, 'digest'):
            self.check()

    def test_tampered_signature(self):
        p = self.directory / 'receipt.bin'
        b = bytearray(p.read_bytes()); b[-1] ^= 1; p.write_bytes(b)
        with self.assertRaisesRegex(ValueError, 'signature'):
            self.check()

    def test_wrong_identity_version_and_lifecycle(self):
        p = self.directory / 'receipt.bin'
        original = p.read_bytes()
        for offset in (4, 8, 12, 16, 20, 24, 28, 32, 64, 112, 176):
            with self.subTest(offset=offset):
                b = bytearray(original); b[offset] ^= 1; p.write_bytes(b)
                with self.assertRaises(ValueError):
                    self.check()

    def test_receipt_truncation_and_trailing_data(self):
        p = self.directory / 'receipt.bin'
        original = p.read_bytes()
        for size in (0, 31, 192, 255, 257):
            p.write_bytes((original + b'x')[:size])
            with self.subTest(size=size), self.assertRaisesRegex(ValueError, '256'):
                self.check()

    def test_wrong_trusted_key(self):
        private = self.directory / 'untrusted-private.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(private)],
                       check=True, capture_output=True)
        subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout', '-out', str(self.key)],
                       check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, 'signature'):
            self.check()

    def test_manifest_and_release_identity(self):
        manifest = tomllib.loads((ROOT / 'Canopus.toml').read_text())
        metadata = json.loads((PAYLOAD / 'release.json').read_text())
        self.assertEqual(manifest['module']['id'], 'ng.lst.corona')
        self.assertEqual(manifest['module']['version'], '0.3.0')
        self.assertEqual(metadata['project_id'], manifest['module']['id'])
        self.assertEqual(metadata['runtime_id'], verifier.MODULE_ID)
        self.assertEqual(metadata['version'], manifest['module']['version'])
        self.assertEqual(metadata['physical_device'], 'NOT_PROBED')
        self.assertEqual(metadata['target'], TARGET)
        receipt = (PAYLOAD / 'receipt.bin').read_bytes()
        self.assertEqual(receipt[64:112], TARGET.encode().ljust(48, b'\0'))
        self.assertEqual(receipt[112:144].hex(), verifier.TARGETS[TARGET])
        self.assertEqual(struct.unpack_from('<I', (PAYLOAD / 'receipt.bin').read_bytes(), 20)[0],
                         metadata['receipt_module_version'])

    def test_existing_output_is_never_overwritten(self):
        marker = self.directory / 'keep'
        marker.write_text('keep this')
        result = subprocess.run(['sh', str(ROOT / 'scripts/build-install-payload.sh'),
                                 TARGET, str(self.directory)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('already exists', result.stderr)
        self.assertEqual(marker.read_text(), 'keep this')

    def test_wrong_target_rejected_before_build(self):
        output = self.directory / 'absent'
        result = subprocess.run(['sh', str(ROOT / 'scripts/build-install-payload.sh'),
                                 'xiaomi-band-11-4.100.108', str(output)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unsupported target', result.stderr)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
