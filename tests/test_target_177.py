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
            0x0c3a3ec0: (68, 'a498eba8b0b9075ddc6f9e549336cd23315d45399d17e2ed6ebb2b76bf627e07'),
            0x0c381e66: (72, 'b48003ab99a6895cbcf7e65f7983f903a0b28062e4b6310bc72e0716b682afb4'),
            0x0c381d4c: (218, 'bd0cc1f042f941a3335cc26df6c56162c108d773222770c1c52a985ae4c69319'),
            0x0c383f00: (56, '324435ff22516191c749e69e1f7eb04a6ac5386551579c39e78d27ff9ed12148'),
            0x0c91e58c: (94, '566f5d7dc2d19222bc4f3f7eddeb65a4fdab7d5b3882c1cf8fe77916a34fccb0'),
            0x0c92e5f4: (20, 'e151f6c922c49f59976e22d90c2488ec5ee0fce08ffe19d909002a796933cc84'),
            0x0c721248: (8, '1c1b157c1c2bdd78f2f21b23e585316448b4d47d83e16f5f4b80e3c5b187e91a'),
            0x0c3894d0: (1816, 'ce13f06888614813801e2d848ce8e92d1792599030f6270b5a66bd23a00b3570'),
            0x0c38928c: (570, 'b3480293da196336dc2ade3da1292b4342fa5193b43001932f4d97782ca7033d'),
            0x0c386c14: (178, 'ba3c620812042fb1c6955fcf594a67a024b08fedaecb9cc1f21fcbb32fd00c41'),
            0x0c386904: (106, 'e9fc0c738f391473a8d6e90d9a5ee1fe10422c394aab03c90c8d6a915c9510bb'),
            0x0c384528: (196, 'f8af3d771a661d93a5c21871532fb59cad700ec8976089609368a67da2ea02ed'),
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
            'OBJECT_EVENT_ADD': 0x0c37fbed, 'OBJECT_EVENT_REMOVE': 0x0c37fc33,
            'RECT_INIT': 0x0c38e03d, 'RECT_DRAW': 0x0c38e47d,
            'DISPLAY_EVENT_ADD': 0x0c3a3ec1, 'DISPLAY_EVENT_REMOVE': 0x0c381e67,
            'LAYOUT_UPDATE': 0x0c383f01, 'VISIBLE_AREA': 0x0c381d4d,
            'GPU_FINISH': 0x0c91e58d, 'DCACHE_SYNC': 0x0c92e5f5,
            'DCACHE_INVALIDATE': 0x0c721249,
            'SNAPSHOT_TAKE': 0x0c5f126d, 'SNAPSHOT_FREE': 0x0c8f29ed,
            'DRAW_BUF_CREATE': 0x0c386c15, 'DRAW_BUF_HANDLERS': 0x200bd18c,
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

    def test_background_refresh_records_remain_private_and_unprobed(self):
        target = 'xiaomi-band-11-4.100.177'
        evidence_id = 'EVID-RESOURCE-BACKGROUND-REFRESH-177-001'
        roles = {
            'background_event_list_add': 0x0c3a3ec0,
            'background_display_event_remove': 0x0c381e66,
            'background_visible_area': 0x0c381d4c,
            'background_layout_update': 0x0c383f00,
            'background_gpu_finish': 0x0c91e58c,
            'background_dcache_sync': 0x0c92e5f4,
            'background_dcache_invalidate': 0x0c721248,
        }
        for role, entry in roles.items():
            with self.subTest(role=role):
                path = PACK / 'symbols' / f'{target}.private_abi.{role}.json'
                record = json.loads(path.read_text())
                self.assertEqual(int(record['entry_address'], 16), entry)
                self.assertEqual(int(record['callable_address'], 16), entry | 1)
                self.assertEqual(record['target_id'], target)
                self.assertEqual(record['policy'], 'restricted')
                self.assertEqual(record['approval_state'], 'PENDING')
                self.assertEqual(record['status'], 'STATIC_RECOVERED')
                self.assertEqual(record['proof']['device'], 'not_probed')
                self.assertEqual(record['proof']['evidence_ids'], [evidence_id])
                self.assertIn('target-private-ui-owner', record['contexts']['allowed'])
                self.assertEqual(record['provenance']['firmware_sha256'],
                    'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b')
        evidence = json.loads((PACK / 'evidence' / f'{evidence_id}.json').read_text())
        self.assertEqual(evidence['verdict'], 'STATIC_RECOVERED')
        self.assertCountEqual(evidence['candidate_symbols'],
            [f'{target}.private_abi.{role}' for role in roles])
        self.assertTrue(any('ROM' in assumption for assumption in evidence['unsafe_assumptions']))

    def test_screen_background_abi_records(self):
        roles = {
            'background_object_event_add': (0x0c37fbec, 38, '5c7523af00abf015e2d3359ef9fc2b5114df295d9fd04a6efd1d7064c7c987d0'),
            'background_object_event_remove': (0x0c37fc32, 62, '0a0881f3bf438263211967761aa5dc17e547592ebecb702725e2284287f3f5ce'),
            'background_rect_init': (0x0c38e03c, 186, 'ee8fa724993a662037349d1c043e900f8ec4269cbdc0a59ce5096d537d3c076c'),
            'background_rect_draw': (0x0c38e47c, 1246, '6b9094a17eadea647f445ac1680191cf4edb3561e675f5686413bc1e172c31dd'),
        }
        data = (ROOT.parent.parent / 'mifw/analysis-q66-177/177/vela_ap.bin').read_bytes()
        evidence_id = 'EVID-RESOURCE-BACKGROUND-SCREEN-177-001'
        for role, (entry, size, digest) in roles.items():
            record = json.loads((PACK / 'symbols' /
                f'xiaomi-band-11-4.100.177.private_abi.{role}.json').read_text())
            self.assertEqual(int(record['entry_address'], 16), entry)
            self.assertEqual(int(record['callable_address'], 16), entry | 1)
            self.assertEqual(record['proof']['evidence_ids'], [evidence_id])
            self.assertEqual(record['proof']['device'], 'not_probed')
            self.assertIn('target-private-ui-owner', record['contexts']['allowed'])
            offset = entry - 0x0c0c0000
            self.assertEqual(hashlib.sha256(data[offset:offset + size]).hexdigest(), digest)
        evidence = json.loads((PACK / 'evidence' / f'{evidence_id}.json').read_text())
        self.assertEqual(len(evidence['candidate_symbols']), 4)

    def test_manifest_and_delivery_pin_identity(self):
        import runpy
        targets = runpy.run_path(str(ROOT / 'scripts/verify-payload.py'))['TARGETS']
        self.assertEqual(targets['xiaomi-band-11-4.100.177'],
            'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b')
        self.assertIn('xiaomi-band-11-4.100.177', (ROOT / 'Canopus.toml').read_text())

if __name__ == '__main__':
    unittest.main()
