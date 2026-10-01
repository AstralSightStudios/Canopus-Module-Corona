"""Exact-target QuickApp allowlist regressions (stdlib Python only).

Run: python3 tests/test_quickapp_allowlist.py
Requires the existing Canopus CLI, clang's ARM backend and ld.lld; missing
prerequisites fail, rather than silently skipping verification. CANOPUS_ROOT,
CANOPUS_CLI, CLANG and LD_LLD follow scripts/build.sh. All ELF probes and
negative-control target packs are temporary: no delivered/build ELF is used.
This checks static verification only, not approval or physical-device success.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SDK = ROOT.parent / 'Canopus'
if not DEFAULT_SDK.is_dir():
    DEFAULT_SDK = ROOT.parent / 'Canopus-Private'
SDK = Path(os.environ.get('CANOPUS_ROOT', DEFAULT_SDK)).resolve()
EVIDENCE_ID = 'EVID-RESOURCE-QUICKAPP-ICON-001'
TARGETS = {
    'xiaomi-band-11-4.100.139': (
        0x0c6a16a2, 0x0c6a16a3,
        '31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74'),
    'xiaomi-band-11-4.100.155': (
        0x0c6a1692, 0x0c6a1693,
        'ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f'),
}


def required_tool(variable, default):
    name = os.environ.get(variable, str(default))
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f'Missing tool: {name} (set {variable})')
    return path


class QuickAppAllowlist(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cli = required_tool('CANOPUS_CLI', SDK / 'target/debug/canopus')
        cls.clang = required_tool('CLANG', 'clang')
        cls.linker = required_tool('LD_LLD', 'ld.lld')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rh-quickapp-allowlist-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        cls.probes = {}
        # No headers, imports, local arrays or implicit __aeabi_memclr. The
        # literal is called, not merely placed in unused data. Pass the caller's
        # package pointer through so the call cannot be constant-folded away.
        addresses = {address + delta for _, address, _ in TARGETS.values()
                     for delta in (-2, 0, 2)}
        for address in sorted(addresses):
            source = cls.work / f'probe-{address:x}.c'
            obj = source.with_suffix('.o')
            elf = source.with_suffix('.elf')
            source.write_text(
                'typedef unsigned int address_t;\n'
                'unsigned int quickapp_probe(const char *package) {\n'
                '    return ((unsigned int (*)(const char *))(address_t)'
                f'0x{address:08x}u)(package);\n'
                '}\n')
            subprocess.run([cls.clang, '--target=arm-none-eabi',
                '-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=soft',
                '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                '-fno-unwind-tables', '-Os', '-Wall', '-Wextra', '-Werror',
                '-c', str(source), '-o', str(obj)], check=True)
            subprocess.run([cls.linker, '-r', str(obj), '-o', str(elf)], check=True)
            cls.probes[address] = elf

    def run_cli(self, *args):
        return subprocess.run([self.cli, *map(str, args)], text=True,
                              capture_output=True)

    def symbol_path(self, target):
        return (SDK / 'targets' / target / 'symbols' /
                f'{target}.app_lookup_package.json')

    def evidence_path(self, target):
        return SDK / 'targets' / target / 'evidence' / f'{EVIDENCE_ID}.json'

    def test_symbol_schema_exact_addresses_and_unpromoted_provenance(self):
        for target, (entry, callable_address, firmware_hash) in TARGETS.items():
            with self.subTest(target=target):
                path = self.symbol_path(target)
                symbol = json.loads(path.read_text())
                result = self.run_cli('symbol', 'validate', path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                expected = {'schema': 1, 'target_id': target,
                    'symbol_id': f'{target}.app_lookup_package',
                    'name': 'app_lookup_package', 'kind': 'function',
                    'instruction_set': 'thumb', 'calling_convention': 'arm-aapcs',
                    'policy': 'restricted', 'status': 'STATIC_RECOVERED',
                    'approval_state': 'PENDING'}
                for key, value in expected.items():
                    self.assertEqual(symbol[key], value, key)
                self.assertEqual(int(symbol['entry_address'], 16), entry)
                self.assertEqual(int(symbol['callable_address'], 16), callable_address)
                self.assertEqual(entry | 1, callable_address)
                self.assertIn('const char *', symbol['prototype'])
                self.assertTrue(symbol['contexts']['allowed'])
                self.assertFalse(symbol['contexts']['blocking'])
                self.assertTrue(symbol['ownership']['argument'])
                self.assertTrue(symbol['ownership']['return_value'])
                self.assertEqual(symbol['proof']['static'], 'recovered')
                self.assertEqual(symbol['proof']['device'], 'not_probed')
                self.assertEqual(symbol['proof']['evidence_ids'], [EVIDENCE_ID])
                self.assertEqual(symbol['provenance']['firmware_sha256'], firmware_hash)
                self.assertEqual(symbol['provenance']['evidence_ids'], [EVIDENCE_ID])
                self.assertTrue(symbol['provenance']['source'])
                self.assertNotIn('promotion', symbol)

    def test_evidence_schema_exact_firmware_and_host_firmware_links(self):
        for target, (_, _, firmware_hash) in TARGETS.items():
            with self.subTest(target=target):
                path = self.evidence_path(target)
                evidence = json.loads(path.read_text())
                result = self.run_cli('evidence', 'validate', path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(evidence['schema'], 1)
                self.assertEqual(evidence['target_id'], target)
                self.assertEqual(evidence['evidence_id'], EVIDENCE_ID)
                self.assertEqual(evidence['verdict'], 'STATIC_RECOVERED')
                self.assertEqual(evidence['candidate_symbols'],
                                 [f'{target}.app_lookup_package'])
                self.assertTrue(evidence['question'])
                self.assertTrue(evidence['ownership_analysis'])
                self.assertTrue(evidence['unsafe_assumptions'])
                self.assertTrue(evidence['recommended_probe'])
                self.assertNotIn('user_device_validation', evidence)
                artifacts = evidence['artifacts']
                ap = [a for a in artifacts if a.get('sha256') == firmware_hash]
                self.assertTrue(ap, 'Evidence must fingerprint the exact AP input')
                for filename in ('tests/test_quickapp_icon_native.py',
                                 'tests/firmware_quickapp_icon.py'):
                    links = [a for a in artifacts if a['uri'].endswith(filename)]
                    self.assertTrue(links, f'Missing evidence link: {filename}')
                    self.assertTrue(all(a.get('role') for a in links))

    def test_firmware_ranges_remain_exact_not_expanded(self):
        for target, (_, _, firmware_hash) in TARGETS.items():
            with self.subTest(target=target):
                manifest = tomllib.loads(
                    (SDK / 'targets' / target / 'target.toml').read_text())
                self.assertEqual(manifest['target_id'], target)
                self.assertEqual(manifest['firmware_sha256'], firmware_hash)
                self.assertEqual(manifest['firmware_address_ranges'],
                                 [{'base': 0x0c000000, 'size': 0x00d00000}])

    def verify_probe(self, target, address, targets_dir=None):
        result = self.run_cli('verify', self.probes[address], '--target', target,
            '--targets-dir', targets_dir or SDK / 'targets', '--json')
        try:
            report = json.loads(result.stdout)
        except json.JSONDecodeError:
            self.fail(f'CLI did not emit a verification report: '
                      f'{result.stdout}\n{result.stderr}')
        summary = report['summary']
        self.assertEqual(summary['format'], 'Elf')
        self.assertEqual(summary['machine'], 'Arm')
        self.assertEqual(summary['kind'], 'Relocatable')
        self.assertEqual(summary['undefined_symbols'], 0)
        self.assertEqual(summary['endianness'], 'little')
        return result, report

    def assert_address_rejected(self, target, address, targets_dir=None):
        result, report = self.verify_probe(target, address, targets_dir)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertFalse(report['ok'])
        # A failure for imports/relocations/format is not an allowlist guard.
        self.assertEqual(report['summary']['absolute_address_hits'], 1)
        self.assertEqual(len(report['errors']), 1, report['errors'])
        error = report['errors'][0]
        self.assertIn('embedded absolute address', error)
        self.assertIn(f'0x{address:x}', error)
        self.assertIn('not in target pack allowlist', error)

    def test_cli_accepts_only_exact_target_callable(self):
        for target, (_, address, _) in TARGETS.items():
            with self.subTest(target=target, address=hex(address)):
                result, report = self.verify_probe(target, address)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(report['ok'])
                self.assertEqual(report['errors'], [])
                self.assertEqual(report['summary']['absolute_address_hits'], 0)
            other = next(row[1] for name, row in TARGETS.items() if name != target)
            for rejected in (address - 2, address + 2, other):
                with self.subTest(target=target, rejected=hex(rejected)):
                    self.assert_address_rejected(target, rejected)

    def test_cli_rejects_callable_without_its_exact_symbol_record(self):
        # Deletion is only in a temporary copy. This causal control prevents a
        # broad range exemption or disabled scanner from making positives pass.
        targets_dir = self.work / 'without-quickapp-symbol'
        for target, (_, address, _) in TARGETS.items():
            with self.subTest(target=target):
                pack = targets_dir / target
                shutil.copytree(SDK / 'targets' / target, pack)
                (pack / 'symbols' / self.symbol_path(target).name).unlink()
                self.assert_address_rejected(target, address, targets_dir)


if __name__ == '__main__':
    unittest.main(verbosity=2)
