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

    def test_manifest_and_delivery_pin_identity(self):
        import runpy
        targets = runpy.run_path(str(ROOT / 'scripts/verify-payload.py'))['TARGETS']
        self.assertEqual(targets['xiaomi-band-11-4.100.177'],
            'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b')
        self.assertIn('xiaomi-band-11-4.100.177', (ROOT / 'Canopus.toml').read_text())

if __name__ == '__main__':
    unittest.main()
