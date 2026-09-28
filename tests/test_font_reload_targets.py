"""Bind the shared C transaction to all 45 audited identities on both APs.

Preprocess the real source so target macros cannot silently hide stale .155
literals. The receipt's source path/hash describe the historical implementation;
its full address set remains the independent oracle for the current algorithm.
No firmware binaries or native-emulation dependencies are needed here.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
AUDIT = json.loads((ROOT / 'targets/xiaomi-band-11-4.100.139/font-reload-compatibility.json').read_text())


def identity(address):
    # Callable target-header entries carry Thumb bits; raw call sites may not.
    return address & ~1 if 0x0c000000 <= address < 0x0d000000 else address


def source_addresses(target, opt_in):
    command = [os.environ.get('CC', 'cc'), '-E', '-P', '-I', str(ROOT / 'include')]
    if target == '155':
        command.append('-DRH_TARGET_155=1')
    if opt_in is not None:
        command.append(f'-DRH_EXPERIMENTAL_FONT_RELOAD={opt_in}')
    command.append(str(ROOT / 'src/font_reload.c'))
    source = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    literals = (int(value, 16) for value in re.findall(r'\b0x([0-9a-fA-F]+)[uUlL]*\b', source))
    return {identity(value) for value in literals if
            0x0c000000 <= value < 0x0d000000 or
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

    def test_release_payload_rejects_opt_in_for_both_targets(self):
        for target in ('139', '155'):
            result = subprocess.run(
                ['sh', str(ROOT / 'scripts/build-install-payload.sh'),
                 f'xiaomi-band-11-4.100.{target}'],
                env=dict(os.environ, RH_EXPERIMENTAL_FONT_RELOAD='1'),
                capture_output=True, text=True)
            with self.subTest(target=target):
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Experimental fonts are not a release payload', result.stderr)

    def test_all_audited_addresses_for_each_target(self):
        original = {identity(int(value, 16)) for value in AUDIT['source_contract']['address_literals']}
        mapping = {int(key, 16): int(value, 16)
                   for key, value in AUDIT['address_map_155_to_139'].items()}
        self.assertEqual(len(original), 45)
        self.assertEqual(sum(mapping[address] != address for address in original), 9)
        for target in ('139', '155'):
            expected = {mapping[address] for address in original} if target == '139' else original
            with self.subTest(target=target):
                self.assertEqual(source_addresses(target, 1), expected)

    def test_no_native_addresses_without_opt_in(self):
        for target in ('139', '155'):
            for opt_in in (None, 0):
                with self.subTest(target=target, opt_in=opt_in):
                    self.assertEqual(source_addresses(target, opt_in), set())


if __name__ == '__main__':
    unittest.main(verbosity=2)
