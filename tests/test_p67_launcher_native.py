#!/usr/bin/env python3
"""Static boundaries for exact-target experimental live launcher adapter.
These do NOT establish device rendering, frame time, or callback ABI acceptance.
"""
from pathlib import Path
import unittest
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

class P67LiveLauncher(unittest.TestCase):
    def test_mocked_native_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix='rh-p67-live-') as directory:
            binary = Path(directory) / 'test'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall',
                            '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'include'),
                            str(ROOT / 'tests/p67_launcher_harness.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    def test_four_surface_native_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix='rh-p67-four-') as directory:
            binary = Path(directory) / 'test'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall',
                            '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'include'),
                            str(ROOT / 'tests/p67_backgrounds_harness.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    def test_gate_and_default(self):
        platform = (ROOT / 'src/platform.c').read_text()
        self.assertIn('#elif defined(RH_TARGET_1043) && RH_TARGET_1043 && defined(RH_P67_LIVE_LAUNCHER) && RH_P67_LIVE_LAUNCHER', platform)
        self.assertIn('#include "p67_launcher.inc"', platform)
        self.assertIn('return (bg && (bg->launcher[0]', platform)
    def test_no_pool_or_snapshot_mutations(self):
        source = (ROOT / 'src/p67_launcher.inc').read_text().lower()
        for address in ['0x0c4b6aa', '0x0c4b6b3', '0x0ca5fb4', '0x1c03163', '0x1c0319c']:
            self.assertNotIn(address, source)
        self.assertIn('load32(p67_image_slot)', source)
        self.assertNotIn('*(volatile uint32_t *)(uintptr_t)p67_image_slot', source)
    def test_forwarding_and_descriptor_identity(self):
        source = (ROOT / 'src/p67_launcher.inc').read_text()
        self.assertIn('page == p67_page && code == 9u', source)
        self.assertIn('P67_NATIVE(P67_SCROLL_CB))(event)', source)
        self.assertIn('d[0] = P67_SCROLL_CB', source)
        self.assertNotIn('d[1] =', source)
        self.assertNotIn('d[2] =', source)
        self.assertIn('if (match) return 0;', source)
        self.assertIn('event + 12) != p67_generation', source)
    def test_exact_rectangle_and_screen_coordinates(self):
        source = (ROOT / 'src/p67_launcher.inc').read_text()
        self.assertIn('uint32_t descriptor[47]', source)
        self.assertIn('descriptor[30] =', source)
        self.assertIn('{0, 0, 335, 479}', source)
        self.assertIn('code != 25u', source)
        self.assertIn('load32(list) != 0x2cd2024cu', source)

if __name__ == '__main__':
    unittest.main()
