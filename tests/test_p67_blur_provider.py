#!/usr/bin/env python3
"""Actual p67 native-provider seams; not hardware cache/GPU acceptance."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeBlurProvider(unittest.TestCase):
    def test_active_snapshot_publication_and_retirement(self):
        with tempfile.TemporaryDirectory(prefix="rh-p67-blur-provider-") as directory:
            binary = Path(directory) / "test"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I" + str(ROOT / "include"),
                str(ROOT / "tests/p67_blur_provider_harness.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
