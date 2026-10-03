"""Default font target contracts and signed installer packaging consistency.

Preprocess the real source against the independent historical Band11 address
oracle. Installer marker tests check routing/byte fidelity only, not ELF or
signature validity. No firmware binaries or native emulation are needed here.
"""
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET_IDS = ('xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155',
              'xiaomi-band-10-pro-3.101.043')
INSTALLER = runpy.run_path(str(ROOT / 'scripts/build-watchface.py'))
AUDIT = json.loads((ROOT / 'targets/xiaomi-band-11-4.100.139/font-reload-compatibility.json').read_text())
AUDIT_1043_FILE = ROOT / 'targets/xiaomi-band-10-pro-3.101.043/font-reload-audit.json'


def identity(address):
    # Callable target-header entries carry Thumb bits; raw call sites may not.
    return address & ~1 if (0x0c000000 <= address < 0x0d000000 or
                           0x1c000000 <= address < 0x1d000000) else address


def source_addresses(target):
    command = [os.environ.get('CC', 'cc'), '-E', '-P', '-I', str(ROOT / 'include')]
    if target == '1043':
        command.append('-DRH_TARGET_1043=1')
    elif target == '155':
        command.append('-DRH_TARGET_155=1')
    command.append(str(ROOT / 'src/font_reload.c'))
    source = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    literals = (int(value, 16) for value in re.findall(r'\b0x([0-9a-fA-F]+)[uUlL]*\b', source))
    return {identity(value) for value in literals if
            0x0c000000 <= value < 0x0d000000 or
            0x1c000000 <= value < 0x1d000000 or
            0x20000000 <= value < 0x20200000 or
            0x2c000000 <= value < 0x2d000000}


class FontTargetContract(unittest.TestCase):
    def test_nine_target_macros_preserve_role_and_thumb_bits(self):
        original = {
            'RH_FW_HEADER_CACHE_CLASS': 0x2ca16934,
            'RH_FW_IMAGE_CACHE_CLASS': 0x2ca168b4,
            'RH_FW_VECTOR_CLASS': 0x2ca6ee48,
            'RH_FW_VECTOR_COMPARE': 0x0c69feb1,
            'RH_FW_VECTOR_DESTROY': 0x0c6a12d5,
            'RH_FW_VECTOR_DROP': 0x0c6a1305,
            'RH_FW_FONT_SET_PIXEL_SIZE': 0x0c8b8a55,
            'RH_FW_CACHE_DROP': 0x0c8b8c9f,
            'RH_FW_CACHE_RELEASE': 0x0c8b9781,
        }
        mapping = {int(key, 16): int(value, 16)
                   for key, value in AUDIT['address_map_155_to_139'].items()}
        for target in ('139', '155'):
            command = [os.environ.get('CC', 'cc'), '-E', '-dM', '-x', 'c',
                       f'-DRH_TARGET_155={int(target == "155")}',
                       str(ROOT / 'include/resource_hook_target.h')]
            source = subprocess.run(command, check=True, capture_output=True, text=True).stdout
            macros = {name: int(value, 16) for name, value in re.findall(
                r'^#define (RH_FW_\w+) (0x[0-9a-fA-F]+)u$', source, re.M)}
            for name, address in original.items():
                expected = (mapping[identity(address)] | (address & 1)) if target == '139' else address
                with self.subTest(target=target, macro=name):
                    self.assertEqual(macros[name], expected)

    def test_face_payload_size_and_vector_drop_abi(self):
        for target in ('139', '155', '1043'):
            command = [os.environ.get('CC', 'cc'), '-E', '-dM', '-x', 'c',
                       f'-DRH_TARGET_155={int(target == "155")}',
                       f'-DRH_TARGET_1043={int(target == "1043")}',
                       str(ROOT / 'include/resource_hook_target.h')]
            source = subprocess.run(command, check=True, capture_output=True, text=True).stdout
            macros = dict(re.findall(r'^#define (RH_FW_FR_\w+) (\d+)u?$', source, re.M))
            with self.subTest(target=target):
                self.assertEqual(int(macros['RH_FW_FR_FACE_SIZE']), 24 if target == '1043' else 28)
                self.assertEqual(int(macros['RH_FW_FR_VECTOR_DROP_SECOND_ARG']), int(target == '1043'))

    def test_all_audited_addresses_for_each_target(self):
        original = {identity(int(value, 16)) for value in AUDIT['source_contract']['address_literals']}
        mapping = {int(key, 16): int(value, 16)
                   for key, value in AUDIT['address_map_155_to_139'].items()}
        self.assertEqual(len(original), 45)
        self.assertEqual(sum(mapping[address] != address for address in original), 9)
        for target in ('139', '155'):
            expected = {mapping[address] for address in original} if target == '139' else original
            with self.subTest(target=target):
                self.assertEqual(source_addresses(target), expected)

    def test_043_static_address_oracle_including_psram(self):
        audit = json.loads(AUDIT_1043_FILE.read_text())
        raw_mapping = audit.get('address_map_155_to_043', audit.get('map_baseline155_to_043'))
        self.assertIsNotNone(raw_mapping, 'static .043 audit must provide the complete address map')
        mapping = {identity(int(key, 16)): identity(int(value, 16))
                   for key, value in raw_mapping.items()}
        original = {identity(int(value, 16)) for value in AUDIT['source_contract']['address_literals']}
        self.assertEqual(set(mapping), original)
        self.assertEqual(len(mapping), 45)
        expected = set(mapping.values())
        self.assertEqual(sum(0x1c000000 <= value < 0x1d000000 for value in expected), 18)
        self.assertEqual(source_addresses('1043'), expected)

    def test_no_experimental_gate_in_standard_build(self):
        for name in ('src/module.c', 'src/font_reload.c', 'scripts/build.sh',
                     'scripts/build-install-payload.sh', 'scripts/build-watchface.py'):
            source = (ROOT / name).read_text()
            with self.subTest(path=name):
                self.assertNotIn('RH_EXPERIMENTAL_FONT_RELOAD', source)
                self.assertNotIn('resource-hook-font-experimental.elf', source)
                self.assertNotIn('resource-hook-0.3.0-font-exp', source)


class InstallerContract(unittest.TestCase):
    def test_defaults_stay_dual_band11(self):
        self.assertEqual(INSTALLER['DEFAULT_TARGETS'], list(TARGET_IDS[:2]))
        self.assertEqual(INSTALLER['TARGETS'], list(TARGET_IDS))
        prefix = 'module-installer-resource-hook-0.3.0-'
        name = INSTALLER['default_output_name']
        self.assertEqual(name(TARGET_IDS[:2]), prefix + 'band11')
        self.assertEqual(name(list(reversed(TARGET_IDS[:2]))), prefix + 'band11')
        self.assertEqual(name([TARGET_IDS[0]]), prefix + 'band11-4.100.139')
        self.assertEqual(name([TARGET_IDS[1]]), prefix + 'band11-4.100.155')
        self.assertEqual(name([TARGET_IDS[2]]), prefix + 'band10pro-3.101.043')
        self.assertEqual(name(TARGET_IDS), prefix + 'multi-device')

    def test_device_routing(self):
        groups = INSTALLER['device_groups'](TARGET_IDS)
        self.assertEqual(groups, {'xiaomi-band-11': list(TARGET_IDS[:2]),
                                  'xiaomi-band-10-pro': [TARGET_IDS[2]]})
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            INSTALLER['device_groups'](['xiaomi-band-10-pro-3.101.036'])

    def test_duplicate_target_rejected_before_build_or_signing(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / 'scripts/build-watchface.py'),
             '--target', TARGET_IDS[2], '--target', TARGET_IDS[2]],
            capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('duplicate target', result.stderr)

    def test_prebuilt_artifact_bypass_is_not_supported(self):
        # An old normal ELF has the same build ID but may contain the former
        # font stub. Refuse bypass before any certificate/key/file work.
        result = subprocess.run(
            [sys.executable, str(ROOT / 'scripts/build-watchface.py'),
             '--no-build', '--certificate-zip', '/missing-certificate.zip'],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('unrecognized arguments: --no-build', result.stderr)
        self.assertNotIn('certificate ZIP not found', result.stderr)

    def test_per_device_byte_fidelity_and_archive_isolation(self):
        # Opaque text markers exercise packaging consistency only. They are NOT
        # ELF/receipt fixtures and do not prove native or signature acceptance.
        verify = INSTALLER['verify_watchface']
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            modules, receipts = {}, {}
            for target in TARGET_IDS:
                payload = base / 'payload' / target
                payload.mkdir(parents=True)
                modules[target] = payload / 'module-marker.txt'
                receipts[target] = payload / 'receipt-marker.txt'
                modules[target].write_text('opaque module consistency marker: ' + target)
                receipts[target].write_text('opaque receipt consistency marker: ' + target)
            for device_name, targets in INSTALLER['device_groups'](TARGET_IDS).items():
                device = base / device_name
                (device / 'build').mkdir(parents=True)
                (device / 'main.lua').write_text('-- opaque watchface source marker')
                for target in targets:
                    (device / f'resource-hook-{target}.bin').write_bytes(modules[target].read_bytes())
                    (device / f'resource-hook-{target}.cmi.bin').write_bytes(receipts[target].read_bytes())
                manifest = device / 'build/manifest.json'
                manifest.write_text(json.dumps({'targets': {t: {'id': t} for t in targets}}))
                archive = device / 'build/resource-hook-prod.zip'
                with zipfile.ZipFile(archive, 'w') as bundle:
                    for item in device.iterdir():
                        if item.is_file():
                            bundle.write(item, item.name)
                verify(device, targets, modules, receipts, archive_path=archive)
                # Cross-device payloads must not leak into the folder or ZIP.
                foreign = next(t for t in TARGET_IDS if t not in targets)
                extra = device / f'resource-hook-{foreign}.bin'
                extra.write_text('foreign marker')
                with self.assertRaisesRegex(ValueError, 'resource contents'):
                    verify(device, targets, modules, receipts)
                extra.unlink()
                with zipfile.ZipFile(archive, 'a') as bundle:
                    bundle.writestr(extra.name, 'foreign marker')
                with self.assertRaisesRegex(ValueError, 'archive contents'):
                    verify(device, targets, modules, receipts, archive_path=archive)
                # Correct filenames are insufficient if either payload differs.
                for suffix in ('.bin', '.cmi.bin'):
                    item = device / f'resource-hook-{targets[0]}{suffix}'
                    original = item.read_bytes()
                    item.write_text('corrupted consistency marker')
                    with self.assertRaisesRegex(ValueError, 'payload differs'):
                        verify(device, targets, modules, receipts)
                    item.write_bytes(original)
                manifest.write_text(json.dumps({'targets': {foreign: {'id': foreign}}}))
                with self.assertRaisesRegex(ValueError, 'manifest targets differ'):
                    verify(device, targets, modules, receipts)


if __name__ == '__main__':
    unittest.main(verbosity=2)
