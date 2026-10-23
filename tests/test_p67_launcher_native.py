#!/usr/bin/env python3
"""Lifecycle and default-product boundaries for the exact .043 adapter.
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
    def test_blur_cpu_and_injected_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="rh-p67-blur-") as directory:
            binary = Path(directory) / "test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall",
                            "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I" + str(ROOT / "include"),
                            str(ROOT / "tests/p67_blur_harness.c"),
                            "-o", str(binary)], check=True)
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
    def test_default_arm_target_includes_all_surfaces_and_native_provider(self):
        sdk = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus'))
        if not sdk.exists():
            sdk = ROOT.parent / 'Canopus-Private'
        source = subprocess.check_output([
            os.environ.get('CLANG', 'clang'), '--target=arm-none-eabi',
            '-mcpu=cortex-m33', '-mthumb', '-E', '-P', '-DRH_TARGET_1043=1',
            '-I' + str(ROOT / 'include'), '-I' + str(sdk / 'sdk/c'),
            '-I' + str(sdk / 'manager/target/band11'),
            '-I' + str(sdk / 'targets/xiaomi-band-10-pro-3.101.043/generated'),
            str(ROOT / 'src/platform.c')], text=True)
        self.assertIn('p67_surfaces[3]', source)
        self.assertIn('p67_blur_native_copy', source)
        self.assertIn('p67_blur_native_publish', source)
        self.assertNotIn('p67_launcher_stage', source)
        self.assertNotIn('rh_platform_p67_blur_diagnostics', source)
        platform = (ROOT / 'src/platform.c').read_text()
        self.assertIn('#include "p67_backgrounds.inc"', platform)
        self.assertNotIn('#include "p67_launcher.inc"', platform)
        self.assertNotIn('return (bg && (bg->launcher[0]', platform)

    def clean_environment(self):
        retired = ('RH_P67_LIVE_LAUNCHER', 'RH_P67_LIVE_BACKGROUNDS', 'RH_P67_BLUR_NATIVE')
        return {key: value for key, value in os.environ.items() if key not in retired}

    def test_retired_product_flags_are_rejected_even_when_zero(self):
        for name in ('RH_P67_LIVE_LAUNCHER', 'RH_P67_LIVE_BACKGROUNDS', 'RH_P67_BLUR_NATIVE'):
            for value in ('0', '1'):
                with self.subTest(flag=name, value=value):
                    env = self.clean_environment()
                    env[name] = value
                    result = subprocess.run(['sh', str(ROOT / 'scripts/build.sh'),
                        'xiaomi-band-10-pro-3.101.043'], env=env, capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('Retired build flag ' + name, result.stderr)

    def test_diagnostics_requires_exact_target(self):
        env = self.clean_environment()
        env['RH_P67_BLUR_DIAGNOSTICS'] = '1'
        result = subprocess.run(['sh', str(ROOT / 'scripts/build.sh'),
            'xiaomi-band-11-4.100.155'], env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Blur diagnostics requires exact .043', result.stderr)

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
    def test_gpu_blur_uses_low_resolution_image_contract(self):
        source = (ROOT / 'src/p67_launcher.inc').read_text()
        provider = (ROOT / 'src/p67_blur_native.inc').read_text()
        self.assertIn('uint32_t descriptor[28]', source)
        self.assertIn('P67_NATIVE(0x0c5882b9u)', source)
        self.assertIn('P67_NATIVE(0x0c5882a1u)', source)
        self.assertIn('descriptor[7] = source', source)
        self.assertIn('descriptor[12] = descriptor[13] = 4u * 256u', source)
        self.assertIn('descriptor[16] = descriptor[17] = 0', source)
        self.assertIn('P67_BLUR_WIDTH - 1u, P67_BLUR_HEIGHT - 1u', source)
        self.assertIn('p67_draw_background(layer', source)
        self.assertIn('p67_blur_native_output_descriptor', provider)
        self.assertIn('P67_BLUR_WIDTH, P67_BLUR_HEIGHT, 18u, 0u', provider)
        self.assertIn('p67_blur_store_rgb565', provider)
        self.assertNotIn('p67_blur_upscale_rgb565', provider)

    def test_exact_rectangle_and_screen_coordinates(self):
        source = (ROOT / 'src/p67_launcher.inc').read_text()
        self.assertIn('uint32_t descriptor[47]', source)
        self.assertIn('descriptor[30] =', source)
        self.assertIn('{0, 0, 335, 479}', source)
        self.assertIn('code != 25u', source)
        self.assertIn('load32(list) != 0x2cd2024cu', source)

if __name__ == '__main__':
    unittest.main()
