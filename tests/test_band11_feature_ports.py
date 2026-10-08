"""Exact AP receipts for the backports; no device/GPU acceptance implied."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import unittest
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_MEM, ARM_REG_PC

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('139', '155')
SDK_DEFAULT = ROOT.parent / 'Canopus'
if not SDK_DEFAULT.exists():
    SDK_DEFAULT = ROOT.parent / 'Canopus-Private'
SDK = Path(os.environ.get('CANOPUS_ROOT', SDK_DEFAULT))

class FeaturePorts(unittest.TestCase):
    def test_exact_ap_functions_globals_and_selected_macros(self):
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        md.detail = True
        for version in TARGETS:
            with self.subTest(version=version):
                evidence = json.loads((ROOT / f'targets/xiaomi-band-11-4.100.{version}/background-calendar-evidence.json').read_text())
                path = Path(os.environ.get('RH_AP_' + version,
                    str(SDK / f'fwbins/xiaomi-band-11-4.100.{version}/vela_ap.bin')))
                data = path.read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), evidence['ap_sha256'])
                macros = subprocess.check_output([os.environ.get('CLANG', 'clang'), '-E', '-dM', '-I' + str(ROOT / 'include'),
                    '-DRH_TARGET_' + version + '=1', '-include', 'resource_hook_target.h', '-'], input=b'').decode()
                for name, entry in evidence['symbols'].items():
                    macro = name.replace('CAL_', 'RH_CN_', 1) if name.startswith('CAL_') else name
                    value = re.search(r'#define ' + macro + r' (0x[0-9a-f]+)u', macros)
                    self.assertIsNotNone(value, macro)
                    self.assertEqual(int(value[1], 16), int(entry['address'], 16), macro)
                    for block in entry.get('blocks', []):
                        start = int(block['address'], 16) - 0xc0c0000
                        self.assertEqual(hashlib.sha256(data[start:start + block['size']]).hexdigest(), block['sha256'], macro)
                    for witness in entry['witnesses']:
                        if 'bytes' in witness:
                            start = int(witness['address'], 16) - 0x2c0c0000
                            raw = bytes.fromhex(witness['bytes'])
                            self.assertEqual(data[start:start + len(raw)], raw)
                        elif int(entry['address'], 16) >= 0x20000000:
                            pc = int(witness['target_function'], 16) + witness['off']
                            ins = next(md.disasm(data[pc - 0xc0c0000:pc - 0xc0c0000 + 4], pc))
                            operand = next(op for op in ins.operands if op.type == ARM_OP_MEM and op.mem.base == ARM_REG_PC)
                            literal = ((pc + 4) & ~3) + operand.mem.disp
                            self.assertEqual(struct.unpack_from('<I', data, literal - 0xc0c0000)[0], int(entry['address'], 16), macro)
                self.assertIn('#define RH_Q66_CALENDAR 1', macros)
                self.assertIn('#define RH_Q66_BACKGROUND 1', macros)

    def test_selected_headers_have_no_duplicate_macros(self):
        for version in (*TARGETS, '177'):
            names = re.findall(r'^#define (\w+)',
                (ROOT / f'include/resource_hook_q66_{version}.h').read_text(), re.M)
            self.assertEqual(len(names), len(set(names)), version)

    def test_043_remains_disabled(self):
        macros = subprocess.check_output([os.environ.get('CLANG', 'clang'), '-E', '-dM', '-DRH_TARGET_1043=1',
            '-I' + str(ROOT / 'include'), '-include', 'resource_hook_target.h', '-'], input=b'').decode()
        self.assertIn('#define RH_Q66_CALENDAR 0', macros)
        self.assertIn('#define RH_Q66_BACKGROUND 0', macros)
        self.assertNotIn('#define RH_CN_', macros)

if __name__ == '__main__':
    unittest.main(verbosity=2)
