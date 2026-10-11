#!/usr/bin/env python3
"""Battery transparency host checks; not device/GPU acceptance."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class P67Battery(unittest.TestCase):
    def test_black_key_pixels(self):
        with tempfile.TemporaryDirectory(prefix="rh-p67-battery-") as directory:
            binary = Path(directory) / "test"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                "-Werror", "-fsanitize=address,undefined", "-I" + str(ROOT / "include"),
                str(ROOT / "tests/test_p67_battery_pixels.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_resource_refresh_invalidates_before_empty_view_return(self):
        source = (ROOT / "src/platform.c").read_text()
        start = source.index("int rh_platform_refresh_mapped_images(")
        end = source.index("int rh_platform_refresh_images(", start)
        refresh = source[start:end]
        self.assertLess(refresh.index("p67_battery_invalidate();"),
                        refresh.index("if (!current->count"))
        self.assertIn("defined(RH_TARGET_1043)", refresh)

    def test_native_capture_lifecycle_and_scroll(self):
        with tempfile.TemporaryDirectory(prefix="rh-p67-battery-native-") as directory:
            binary = Path(directory) / "test"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                "-Werror", "-fsanitize=address,undefined", "-I" + str(ROOT / "include"),
                str(ROOT / "tests/p67_battery_harness.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
