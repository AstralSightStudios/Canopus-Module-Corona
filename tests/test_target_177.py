"""Exact .177 static identities; no device acceptance is implied."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT.parent / 'Canopus-Private/targets/xiaomi-band-11-4.100.177'

class Target177(unittest.TestCase):
    def test_all_call_constants_match_reviewed_pack(self):
        records = json.loads((PACK / 'evidence/fw-match/port-155-to-177.json').read_text())['symbols']
        addresses = {int(r['target_address'], 16) for r in records if isinstance(r['target_address'], str)}
        macros = subprocess.check_output(['clang', '-E', '-dM', '-DRH_TARGET_177=1',
            '-I' + str(ROOT / 'include'), '-include', 'resource_hook_target.h', '-'], input=b'').decode()
        for name, value in re.findall(r'#define (RH_FW_\w+) (0x[0-9a-f]+)u', macros):
            address = int(value, 16)
            if 0x0c000000 <= address < 0x0d000000:
                with self.subTest(name=name):
                    # Vertical accessor independently decoded immediately after horizontal.
                    self.assertIn(address & ~1, addresses | {0x0c381650})
        self.assertNotIn('#define RH_TARGET_155 ', macros)

    def test_exact_ap_literal_witnesses(self):
        import os
        import struct
        path = Path(os.environ.get('RESOURCE_HOOK_FIRMWARE',
            ROOT.parent.parent / 'mifw/analysis-q66-177/177/vela_ap.bin'))
        data = path.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
            'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b')
        for address, value in {0x0c38164c: 0x200bd08c,
                               0x0c38166c: 0x200bd08c,
                               0x0c3ae130: 0x2ca12d74,
                               0x0c547848: 0x200c5d88,
                               0x0ca3b4c0: 0x0c69fa6b}.items():
            self.assertEqual(struct.unpack_from('<I', data, address - 0x0c0c0000)[0], value)

    def test_launcher_background_static_witnesses(self):
        import os
        path = Path(os.environ.get('RESOURCE_HOOK_FIRMWARE',
            ROOT.parent.parent / 'mifw/analysis-q66-177/177/vela_ap.bin'))
        data = path.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
            'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b')
        witnesses = {
            0x0c381090: (34, '28f095facdda74b2cc04bc2badae8d6da723f564d62da9b2ebaf35dba1e6ebe0'),
            0x0c383434: (304, '7b050a217e8a5e32d0eb821493d3dc42afd95e1190055a61bcf0e616db6e6c11'),
            0x0c542384: (30, '0fb05bc295311d9f17f22c605982fb4896382f30c611ddf0e7772301eace738f'),
            0x0c542744: (66, '3bc557becdf5340550356ab76b7d3539f28bf8dad3698c915cf1676d488b3fb3'),
            0x0c386de2: (1056, 'add4cedc6c19ace4b701fd844109dfa436403278929d20f5000f9731bbfe567a'),
        }
        for address, (size, expected) in witnesses.items():
            offset = address - 0x0c0c0000
            self.assertEqual(hashlib.sha256(data[offset:offset + size]).hexdigest(), expected)
        offset = 0x2ca435bc - 0x2c0c0000
        self.assertEqual(data[offset:offset + 36].hex(),
            'd4baa32c4527540c00000000000000000000000000000000d400000008020000c0051000')
        macros = (ROOT / 'include/resource_hook_target_177.h').read_text()
        for name, address in {
            'STYLE_SET': 0x0c383435,
            'LIST_CLASS': 0x2ca435bc, 'BASE_CLASS': 0x2ca3bad4,
            'MAIN_ROOT_SLOT': 0x200c5f10, 'MAIN_CACHE_SLOT': 0x200c5f0c,
            'MORE_ROOT_SLOT': 0x200c5adc, 'MORE_CACHE_SLOT': 0x200c5ad8,
            'CONTROL_PRESENTER_SLOT': 0x200c5668,
            'NOTIFY_CONTAINER_SLOT': 0x200c633c,
            'NOTIFY_LIST_SLOT': 0x200c6240,
            'NOTIFY_LIST_CLASS': 0x2ca3bab0,
        }.items():
            value = re.search(r'#define RH_LB_' + name + r' (0x[0-9a-f]+)u', macros)
            self.assertIsNotNone(value)
            self.assertEqual(int(value[1], 16), address)

    def test_manifest_and_delivery_pin_identity(self):
        import runpy
        targets = runpy.run_path(str(ROOT / 'scripts/verify-payload.py'))['TARGETS']
        self.assertEqual(targets['xiaomi-band-11-4.100.177'],
            'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b')
        self.assertIn('xiaomi-band-11-4.100.177', (ROOT / 'Canopus.toml').read_text())

if __name__ == '__main__':
    unittest.main()
