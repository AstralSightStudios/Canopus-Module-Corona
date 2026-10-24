"""Exact .179 target selection and independent firmware/pack identity checks.

These are static checks, not physical-device or complete firmware emulation.
Missing local firmware/private packs explicitly skip their dependent checks.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import tomllib
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'xiaomi-band-11-4.100.179'
SDK = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus-Private'))
PACK = SDK / 'targets' / TARGET
AP_SHA256 = '307adabe0ff61c77be73d045a8260e279cddd131c7a535a2678eb34199278a2b'
OTA = ROOT.parent.parent / 'mifw/miwear.watch.q66tc_v4.100.179_full_a93678a9.bin'


class Target179(unittest.TestCase):
    def macros(self, *extra):
        return subprocess.run([os.environ.get('CLANG', 'clang'), '-E', '-dM', '-x', 'c',
                               '-DRH_TARGET_179=1', *extra, '-I' + str(ROOT / 'include'),
                               str(ROOT / 'include/resource_hook_target.h')],
                              capture_output=True, text=True)

    def test_exact_target_does_not_select_legacy_adapter(self):
        result = self.macros()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('#define RH_Q66_BACKGROUND 1', result.stdout)
        self.assertIn('#define RH_Q66_CALENDAR 1', result.stdout)
        self.assertIn('#define RH_FW_FR_FACE_SIZE 28u', result.stdout)
        self.assertNotRegex(result.stdout, r'#define RH_TARGET_(155|177|1043) ')
        for other in ('155', '177', '1043'):
            with self.subTest(other=other):
                result = self.macros('-DRH_TARGET_' + other + '=1')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Select exactly one Resource Hook target', result.stderr)

    def test_build_and_receipt_bind_the_exact_ap(self):
        manifest = tomllib.loads((ROOT / 'Canopus.toml').read_text())
        self.assertIn(TARGET, manifest['targets']['include'])
        verifier = runpy.run_path(str(ROOT / 'scripts/verify-payload.py'))
        compiler = runpy.run_path(str(ROOT / 'scripts/canopus-cc.py'))
        self.assertEqual(verifier['TARGETS'][TARGET], AP_SHA256)
        self.assertEqual(compiler['TARGET_FLAGS'][TARGET], ['-DRH_TARGET_179=1'])
        if not PACK.is_dir():
            self.skipTest('exact .179 Canopus target pack is not installed')
        pack = tomllib.loads((PACK / 'target.toml').read_text())
        self.assertEqual(pack['target_id'], TARGET)
        self.assertEqual(pack['firmware_sha256'], AP_SHA256)
        generated = (PACK / 'generated/canopus_target_config.h').read_text()
        self.assertIn('#define CANOPUS_TARGET_ID "' + TARGET + '"', generated)
        fingerprint = re.search(r'#define CANOPUS_SUP_FIRMWARE_SHA256_BYTES\s+\\\s*\{([^}]+)\}', generated)
        self.assertIsNotNone(fingerprint)
        self.assertEqual(''.join(re.findall(r'0x([0-9a-fA-F]{2})\b', fingerprint[1])).lower(), AP_SHA256)

    def firmware(self):
        override = os.environ.get('RH_AP_179')
        if os.environ.get('RESOURCE_HOOK_TARGET', TARGET) == TARGET:
            override = override or os.environ.get('RESOURCE_HOOK_FIRMWARE')
        if override:
            data = Path(override).read_bytes()
        elif OTA.is_file():
            with zipfile.ZipFile(OTA) as archive:
                names = [name for name in archive.namelist() if Path(name).name == 'vela_ap.bin']
                self.assertEqual(len(names), 1)
                data = archive.read(names[0])
        else:
            self.skipTest('exact .179 firmware is not installed')
        self.assertEqual(hashlib.sha256(data).hexdigest(), AP_SHA256)
        return data

    def test_supplied_ota_contains_the_pinned_ap(self):
        self.firmware()

    def test_every_adapter_constant_has_exact_ap_evidence(self):
        data = self.firmware()
        evidence = json.loads((ROOT / 'targets' / TARGET / 'address-map.json').read_text())
        self.assertEqual(evidence['firmware_sha256'], AP_SHA256)
        self.assertEqual(evidence['unresolved'], [])
        result = self.macros()
        self.assertEqual(result.returncode, 0, result.stderr)
        quickapp = subprocess.check_output([os.environ.get('CLANG', 'clang'), '-E', '-dM',
            '-DRH_TARGET_179=1', '-I' + str(ROOT / 'include'),
            str(ROOT / 'src/quickapp_icon.c')], text=True)
        constants = dict(re.findall(r'^#define (\w+)(?:\([^)]*\))? (.*)$',
                                    result.stdout + quickapp, re.M))
        selected = {name for name in constants if re.match(r'RH_(FW_|LB_|CN_|CALENDAR_)|QA_', name)}
        self.assertEqual(selected, set(evidence['macros']))
        for name, record in evidence['macros'].items():
            with self.subTest(macro=name):
                actual = constants[name]
                if re.fullmatch(r'(0x[0-9a-fA-F]+|[0-9]+)[uUlL]*', actual):
                    self.assertEqual(int(actual.rstrip('uUlL'), 0), int(record['value'], 0))
                else:
                    self.assertEqual(actual, record['value'])
                if 'symbol' in record and PACK.is_dir():
                    symbol = json.loads((PACK / 'symbols' / (record['symbol'] + '.json')).read_text())
                    identities = {int(symbol[key], 16) for key in ('entry_address', 'callable_address')
                                  if isinstance(symbol.get(key), str)}
                    self.assertIn(int(record['value'], 16), identities)
                for witness in record.get('witnesses', []):
                    address = int(witness['address'], 16)
                    expected = bytes.fromhex(witness['bytes'])
                    base = 0x2c0c0000 if address >= 0x2c000000 else 0x0c0c0000
                    offset = address - base
                    self.assertGreaterEqual(offset, 0)
                    self.assertEqual(witness.get('size', len(expected)), len(expected))
                    self.assertEqual(data[offset:offset + len(expected)], expected)
                    if 'sha256' in witness:
                        self.assertEqual(hashlib.sha256(expected).hexdigest(), witness['sha256'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
