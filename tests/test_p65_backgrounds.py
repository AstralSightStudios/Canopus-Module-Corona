"""Compile production P65 owner adapter with modeled firmware leaves."""
import os
import json
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class P65Backgrounds(unittest.TestCase):
    def test_exact_p65_background_witnesses(self):
        from p65_firmware import firmware
        try: data=firmware()
        except FileNotFoundError as error: self.skipTest(str(error))
        records=json.loads((ROOT/'targets/redmi-watch-6-3.100.043/background-functions.json').read_text())
        for name,record in records.items():
            with self.subTest(name=name):
                witness=record['witness'];offset=int(record['file_offset'],0)
                actual=data[offset:offset+witness['size']]
                self.assertEqual(actual.hex(),witness['hex'])
                self.assertEqual(hashlib.sha256(actual).hexdigest(),witness['sha256'])
        audit=json.loads((ROOT/'targets/redmi-watch-6-3.100.043/background-audit.json').read_text())
        for key in ('delete_call_witness','redraw_prechildren_witness'):
            record=audit[key];offset=int(record['file_offset'],0);expected=bytes.fromhex(record['bytes'])
            self.assertEqual(data[offset:offset+len(expected)],expected)

    def test_four_surface_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix='rh-p65-backgrounds-') as directory:
            directory=Path(directory)
            (directory/'blur-test.inc').write_text('''static uintptr_t p65_blur_source(const char *path) {return rh_background_is_blur(path)?0:(uintptr_t)path;}
static void p65_blur_reset(void) {++blur_reset_calls;}
static void p65_blur_watch(unsigned demand) {(void)demand;++blur_polls;}
''')
            executable=directory/'test'
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-DP65_BG_TEST_BLUR="blur-test.inc"',
                '-I'+str(directory),'-I'+str(ROOT/'include'),
                str(ROOT/'tests/p65_backgrounds_harness.c'),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)
if __name__=='__main__':unittest.main(verbosity=2)
