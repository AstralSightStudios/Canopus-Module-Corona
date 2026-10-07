"""Exact .177 calendar-private approvals; never executes firmware calls.

Checks real Canopus schema/policy validation, AP byte receipts, generated private
constant/public-veneer separation and ELF negative controls. No address-range or
verifier override is used. All probe ELFs and withdrawn-record packs are temporary.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SDK = ROOT.parent / 'Canopus'
if not DEFAULT_SDK.exists():
    DEFAULT_SDK = ROOT.parent / 'Canopus-Private'
SDK = Path(os.environ.get('CANOPUS_ROOT', DEFAULT_SDK)).resolve()
TARGET = 'xiaomi-band-11-4.100.177'
SHA = 'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b'
EVID = 'EVID-RESOURCE-CALENDAR-ALPHA-177-001'
BASE = 0x0c0c0000
ENTRIES = {
    'realtime': 0x0c34d45c, 'localtime': 0x0c34fcec,
    'root_style': 0x0c502de0, 'text_color': 0x0c3839ae,
    'text_align': 0x0c383a10, 'text_font': 0x0c3839e4,
    'font_resolve': 0x0c50934c, 'font_create': 0x0c495008,
    'font_destroy': 0x0c4924fc, 'weekday': 0x0c542cdc,
    'snapshot': 0x0c6a29c8, 'snapshot_free': 0x0c6a2a24,
    'object_delete': 0x0c384488, 'premultiply': 0x0c907918,
    'draw_buf_invalidate': 0x0c8b7720,
}


class CalendarAllowlist(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pack = SDK / 'targets' / TARGET
        cls.cli_path = str(SDK / 'target/debug/canopus')
        cls.clang = os.environ.get('CLANG', 'clang')
        cls.linker = os.environ.get('LD_LLD', 'ld.lld')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-calendar-allowlist-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        cls.probes = {}
        for role, entry in ENTRIES.items():
            source = cls.work / (role + '.c')
            obj, elf = source.with_suffix('.o'), source.with_suffix('.elf')
            source.write_text(
                'typedef unsigned int address_t;\n'
                'unsigned int calendar_probe(void *object) {\n'
                f'    unsigned int receipt = *(volatile const unsigned int *)0x{entry:x}u;\n'
                '    return receipt + ((unsigned int (*)(void *))(address_t)'
                f'0x{entry | 1:x}u)(object);\n'
                '}\n')
            subprocess.run([cls.clang, '--target=arm-none-eabi', '-mcpu=cortex-m33',
                '-mthumb', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-unwind-tables', '-Os', '-Wall',
                '-Wextra', '-Werror', '-c', str(source), '-o', str(obj)], check=True)
            subprocess.run([cls.linker, '-r', '-e', 'calendar_probe', str(obj),
                            '-o', str(elf)], check=True)
            cls.probes[role] = elf

    def cli(self, *args):
        return subprocess.run([self.__class__.cli_path, *map(str, args)],
                              capture_output=True, text=True)

    def symbol_path(self, role):
        return self.pack / 'symbols' / f'{TARGET}.private_abi.calendar_{role}.json'

    def test_exact_private_approval_does_not_claim_device_proof(self):
        for role, entry in ENTRIES.items():
            with self.subTest(role=role):
                path = self.symbol_path(role)
                value = json.loads(path.read_text())
                result = self.cli('symbol', 'validate', path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(value['symbol_id'], f'{TARGET}.private_abi.calendar_{role}')
                self.assertEqual(value['target_id'], TARGET)
                self.assertEqual(value['kind'], 'function')
                self.assertEqual(int(value['entry_address'], 16), entry)
                self.assertEqual(int(value['callable_address'], 16), entry | 1)
                self.assertEqual(value['policy'], 'restricted')
                self.assertEqual(value['approval_state'], 'APPROVED')
                self.assertEqual(value['status'], 'STATIC_RECOVERED')
                self.assertEqual(value['proof']['static'], 'recovered')
                self.assertEqual(value['proof']['device'], 'not_probed')
                self.assertEqual(value['proof']['evidence_ids'], [EVID])
                self.assertEqual(value['provenance']['firmware_sha256'], SHA)
                promotion = value['promotion']
                self.assertEqual(promotion['firmware_sha256'], SHA)
                self.assertEqual(promotion['prototype'], value['prototype'])
                self.assertEqual(promotion['thread'], 'target-private-ui-owner')
                self.assertIn('user', promotion['reviewer'].lower())
                self.assertTrue(promotion['date'])
                self.assertTrue(value['ownership']['argument'])
                self.assertTrue(value['ownership']['return_value'])

    def test_evidence_receipts_match_exact_firmware(self):
        path = self.pack / 'evidence' / f'{EVID}.json'
        result = self.cli('evidence', 'validate', path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads(path.read_text())
        self.assertEqual(evidence['verdict'], 'STATIC_RECOVERED')
        self.assertEqual(evidence['target_id'], TARGET)
        self.assertCountEqual(evidence['candidate_symbols'],
            [f'{TARGET}.private_abi.calendar_{role}' for role in ENTRIES])
        firmware = (SDK / 'fwbins' / TARGET / 'vela_ap.bin').read_bytes()
        self.assertEqual(hashlib.sha256(firmware).hexdigest(), SHA)
        artifacts = {a['role']: a for a in evidence['artifacts']}
        for role, entry in ENTRIES.items():
            artifact = artifacts[f'function:{TARGET}.private_abi.calendar_{role}']
            match = re.fullmatch(r'firmware://([0-9a-f]{64})/ap/(0x[0-9a-f]+)/(0x[0-9a-f]+)', artifact['uri'])
            self.assertIsNotNone(match)
            self.assertEqual(match[1], SHA)
            self.assertEqual(int(match[2], 16), entry)
            size = int(match[3], 16)
            self.assertGreater(size, 0)
            self.assertEqual(hashlib.sha256(firmware[entry - BASE:entry - BASE + size]).hexdigest(),
                             artifact['sha256'])

    def test_restricted_records_never_emit_public_wrappers(self):
        c = (self.pack / 'generated/canopus_veneer.h').read_text()
        rust = (SDK / 'sdk/rust/canopus-target-generated/src/generated_1177.rs').read_text()
        for role in ENTRIES:
            name = f'calendar_{role}'
            self.assertNotIn(f'canopus_fw_{name}(', c)
            self.assertNotIn(f'pub unsafe fn canopus_fw_{name}(', rust)
            self.assertIn(f'CANOPUS_FW_{name.upper()}_CALLABLE', rust)

    def test_only_exact_entries_are_permitted(self):
        for role, elf in self.probes.items():
            with self.subTest(role=role):
                result = self.cli('verify', elf, '--target', TARGET, '--targets-dir', SDK / 'targets')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # Removing one exact symbol must re-close the gate, using an unchanged
        # firmware range and unchanged verifier. No real target files are deleted.
        targets = self.work / 'without-calendar'
        shutil.copytree(self.pack, targets / TARGET)
        role = 'snapshot'
        (targets / TARGET / 'symbols' / self.symbol_path(role).name).unlink()
        result = self.cli('verify', self.probes[role], '--target', TARGET, '--targets-dir', targets)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not in target pack allowlist', result.stdout + result.stderr)
        original = tomllib.loads((self.pack / 'target.toml').read_text())
        copied = tomllib.loads((targets / TARGET / 'target.toml').read_text())
        self.assertEqual(copied['firmware_address_ranges'], original['firmware_address_ranges'])
        # Approval applies to .177 only, not to another Band 11 firmware pack.
        result = self.cli('verify', self.probes['snapshot'], '--target',
                          'xiaomi-band-11-4.100.155', '--targets-dir', SDK / 'targets')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not in target pack allowlist', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
