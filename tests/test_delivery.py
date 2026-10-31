"""Delivery checks require a built payload; failures are not skipped."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import subprocess
import tempfile
import tomllib
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TARGET = os.environ.get('RESOURCE_HOOK_TARGET', 'xiaomi-band-11-4.100.139')
PAYLOAD = Path(os.environ.get('RESOURCE_HOOK_PAYLOAD',
    ROOT / 'build/payload-0.3.0' / TARGET))
spec = importlib.util.spec_from_file_location('verify_payload', ROOT / 'scripts/verify-payload.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
spec = importlib.util.spec_from_file_location('build_delivery', ROOT / 'scripts/build-delivery.py')
delivery_builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(delivery_builder)


class FirmwareSelection(unittest.TestCase):
    def suites(self, target):
        return dict(delivery_builder.firmware_test_commands(target, 'firmware-python'))

    def test_1043_uses_designated_native_suites(self):
        target = 'xiaomi-band-10-pro-3.101.043'
        suites = self.suites(target)
        self.assertEqual(set(suites), {'firmware_font_1043', 'firmware_quickapp_reload_1043',
                                      'firmware_calendar'})
        self.assertEqual(suites['firmware_calendar'][-2:], ['--target', target])
        for name, command in suites.items():
            self.assertEqual(command[:2], ['firmware-python', str(ROOT / 'tests' / (name + '.py'))])

    def test_band11_uses_exact_target_barrier_and_reload(self):
        for target in ('xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155'):
            with self.subTest(target=target):
                suites = self.suites(target)
                self.assertIn('firmware_font_barrier_155', suites)
                self.assertIn('firmware_font_lifecycle', suites)
                self.assertIn('firmware_reload', suites)
                self.assertIn('firmware_image_lifecycle', suites)
                self.assertNotIn('firmware_font_1043', suites)
                self.assertNotIn('firmware_quickapp_reload_1043', suites)
                self.assertEqual(suites['firmware_calendar'][-2:], ['--target', target])

    def test_new_targets_have_no_unrelated_firmware_suites(self):
        self.assertEqual(self.suites('xiaomi-band-11-4.100.177'), {})
        self.assertEqual(set(self.suites('xiaomi-band-11-4.100.179')), {'test_calendar_native'})
        self.assertEqual(set(self.suites('redmi-watch-6-3.100.043')), {'firmware_p65', 'firmware_p65_features', 'firmware_p65_font', 'test_p65_calendar_native', 'test_p65_blur'})

    def test_p65_payload_release_metadata_without_compilation(self):
        script = (ROOT / 'scripts/build-install-payload.sh').read_text()
        block = re.search(r'python3 - "\$STAGE" "\$TARGET_ID" "\$FIRMWARE" "\$ROOT/Canopus.toml" <<\'PY\'\n(.*?)\nPY',
                          script, re.S)
        self.assertIsNotNone(block)
        target = 'redmi-watch-6-3.100.043'
        with tempfile.TemporaryDirectory(prefix='p65-release-metadata-') as temporary:
            command = ['release-metadata', temporary, target, verifier.TARGETS[target],
                       str(ROOT / 'Canopus.toml')]
            with mock.patch.object(sys, 'argv', command), mock.patch.dict(os.environ, {}, clear=True):
                exec(compile(block[1], 'payload-release-metadata', 'exec'), {})
            metadata = json.loads((Path(temporary) / 'release.json').read_text())
        self.assertEqual(metadata['target'], target)
        self.assertEqual(metadata['firmware_sha256'], verifier.TARGETS[target])
        self.assertTrue(metadata['font_reload'])
        self.assertEqual(metadata['font_reload_device_status'], 'NOT_PROBED')
        self.assertEqual(metadata['background_targets'], ['launcher', 'control_center', 'notifications', 'widgets'])
        self.assertEqual(metadata['target_features'][target]['device_status'], 'NOT_PROBED')
        self.assertTrue(metadata['target_features'][target]['calendar'])

    def test_unknown_native_target_refused(self):
        with self.assertRaisesRegex(ValueError, 'unsupported target'):
            self.suites('xiaomi-band-11-4.100.108')


class Delivery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='resource-hook-delivery-test-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        for name in ('receipt.bin', 'resource-hook.elf'):
            shutil.copyfile(PAYLOAD / name, self.directory / name)

    def check(self):
        return verifier.verify(self.directory, TARGET)

    def test_valid_receipt(self):
        self.assertEqual(len(self.check()), 64)

    def test_wrong_expected_target(self):
        other = next(target for target in verifier.TARGETS if target != TARGET)
        with self.assertRaisesRegex(ValueError, 'target ID'):
            verifier.verify(self.directory, other)

    def test_tampered_artifact(self):
        p = self.directory / 'resource-hook.elf'
        b = bytearray(p.read_bytes()); b[-1] ^= 1; p.write_bytes(b)
        with self.assertRaisesRegex(ValueError, 'digest'):
            self.check()

    def test_tampered_receipt_digest(self):
        p = self.directory / 'receipt.bin'
        b = bytearray(p.read_bytes()); b[-1] ^= 1; p.write_bytes(b)
        with self.assertRaisesRegex(ValueError, 'digest'):
            self.check()

    def test_wrong_identity_version_and_lifecycle(self):
        p = self.directory / 'receipt.bin'
        original = p.read_bytes()
        for offset in (0, 4, 8, 12, 16, 20, 24, 28, 32, 64, 112):
            with self.subTest(offset=offset):
                b = bytearray(original); b[offset] ^= 1; p.write_bytes(b)
                with self.assertRaises(ValueError):
                    self.check()

    def test_receipt_truncation_and_trailing_data(self):
        p = self.directory / 'receipt.bin'
        original = p.read_bytes()
        for size in (0, 31, 175, 177, 256):
            p.write_bytes((original + bytes(80))[:size])
            with self.subTest(size=size), self.assertRaisesRegex(ValueError, '176'):
                self.check()

    def test_package_contains_the_verified_payload_and_receipt(self):
        import tarfile
        with tarfile.open(PAYLOAD / 'canopus_corona-0.3.0.canopus') as archive:
            self.assertEqual(archive.extractfile(f'artifacts/{TARGET}/module.elf').read(),
                             (PAYLOAD / 'resource-hook.elf').read_bytes())
            self.assertEqual(archive.extractfile(f'receipts/{TARGET}.cmi').read(),
                             (PAYLOAD / 'receipt.bin').read_bytes())
            manifest = tomllib.loads((ROOT / 'Canopus.toml').read_text())
            for relative in manifest.get('resources', {}).get('files', []):
                resource = ROOT / relative
                self.assertEqual(archive.extractfile('resources/common/' + resource.name).read(),
                                 resource.read_bytes())
        self.assertFalse((PAYLOAD / 'signer-public.pem').exists())

    def test_manifest_and_release_identity(self):
        manifest = tomllib.loads((ROOT / 'Canopus.toml').read_text())
        metadata = json.loads((PAYLOAD / 'release.json').read_text())
        self.assertEqual(manifest['module']['id'], 'canopus_corona')
        self.assertEqual(manifest['module']['version'], '0.3.0')
        self.assertEqual(metadata['project_id'], manifest['module']['id'])
        self.assertEqual(metadata['runtime_id'], verifier.MODULE_ID)
        self.assertEqual(metadata['version'], manifest['module']['version'])
        self.assertEqual(metadata['physical_device'], 'NOT_PROBED')
        p65 = TARGET == 'redmi-watch-6-3.100.043'
        self.assertEqual(metadata['font_reload_device_status'],
                         'NOT_PROBED' if p65 or TARGET.endswith(('4.100.177', '4.100.179')) else 'USER_REPORTED_PASS')
        self.assertEqual(metadata['receipt_format_version'], 2)
        self.assertEqual(metadata['format'], 'elf-cmi2')
        self.assertTrue(metadata['font_reload'])
        if p65:
            self.assertEqual(metadata['target_features'][TARGET], {
                'font_reload': True, 'native_backgrounds': True,
                'calendar': True, 'device_status': 'NOT_PROBED',
            })
            self.assertEqual(metadata['background_targets'], ['launcher', 'control_center', 'notifications', 'widgets'])
            self.assertEqual(metadata['background_device_status'], 'NOT_PROBED')
        self.assertEqual(metadata['module_build_id'], 'resource-hook-0.3.0')
        self.assertEqual(metadata['gpu_recovery'], 'UNSUPPORTED')
        self.assertEqual(metadata['framework_restart_safety'], 'UNSUPPORTED')
        self.assertFalse(metadata['automatic_ui_restart'])
        self.assertFalse(metadata['complete_cache_refresh'])
        self.assertEqual(metadata['target'], TARGET)
        receipt = (PAYLOAD / 'receipt.bin').read_bytes()
        self.assertEqual(receipt[64:112], TARGET.encode().ljust(48, b'\0'))
        self.assertEqual(receipt[112:144].hex(), verifier.TARGETS[TARGET])
        self.assertEqual(struct.unpack_from('<I', (PAYLOAD / 'receipt.bin').read_bytes(), 20)[0],
                         metadata['receipt_module_version'])

    def test_font_docs_evidence_and_recursive_checksums(self):
        self.assertTrue((PAYLOAD / 'FONT_RELOAD.md').is_file())
        source = ROOT / 'targets' / TARGET
        expected = [item for item in source.iterdir()
                    if item.is_file() and item.suffix in ('.json', '.md')]
        if TARGET != 'redmi-watch-6-3.100.043' and not TARGET.endswith(('4.100.177', '4.100.179')):
            self.assertTrue(any(item.name.startswith('font-reload') for item in expected))
        for item in expected:
            self.assertEqual((PAYLOAD / 'evidence' / item.name).read_bytes(), item.read_bytes())
        checksums = (PAYLOAD / 'SHA256SUMS').read_text()
        for line in checksums.splitlines():
            digest, name = line.split('  ', 1)
            with self.subTest(file=name):
                self.assertEqual(hashlib.sha256((PAYLOAD / name).read_bytes()).hexdigest(), digest)
        self.assertIn('  evidence/', checksums)
        self.assertIn('  FONT_RELOAD.md', checksums)

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
