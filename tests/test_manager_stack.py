"""Bound the ARM stack used by empty native-manager pages.

Host sanitizers cannot detect exhaustion of the firmware UI task stack. Compile
production sources for Cortex-M33 and keep the empty list/choice call chains
below 3 KiB, leaving room for the Supervisor, SDK and firmware event frames.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("CANOPUS_ROOT", ROOT.parent / "Canopus-Private"))
CLANG = shutil.which(os.environ.get("CLANG", "clang"))


@unittest.skipUnless(CLANG, "ARM clang is required for stack-usage verification")
class ManagerStack(unittest.TestCase):
    def test_empty_page_stack_budget(self):
        for target in (155, 179, 1043, "P65"):
            with self.subTest(target=target), tempfile.TemporaryDirectory(prefix="corona-stack-") as tmp:
                frames = {}
                for source in ("manager_data", "manager_ui"):
                    output = Path(tmp) / (source + ".o")
                    subprocess.run([
                        CLANG, "--target=arm-none-eabi", "-mcpu=cortex-m33", "-mthumb",
                        "-mfloat-abi=soft", "-Os", "-ffreestanding", "-fno-builtin",
                        "-fstack-usage", "-DRH_NATIVE_MANAGER=1", f"-DRH_TARGET_{target}=1",
                        "-I" + str(ROOT / "include"), "-I" + str(SDK / "sdk/c"),
                        "-c", str(ROOT / "src" / (source + ".c")), "-o", str(output),
                    ], check=True, capture_output=True, text=True)
                    for line in output.with_suffix(".su").read_text().splitlines():
                        location, size, kind = line.split("\t")
                        self.assertEqual(kind, "static", line)
                        frames[location.rsplit(":", 1)[-1]] = int(size)
                for name in ("rh_manager_data_mix_get", "rh_manager_data_option_get"):
                    self.assertLessEqual(frames[name], 1024, f"{name} reserves full package metadata")
                for name in ("ensure_catalog", "theme_available"):
                    if name in frames:
                        self.assertLessEqual(frames[name], 512, f"{name} reserves a full manifest")
                common = sum(frames.get(name, 0) for name in ("on_event", "navigate", "render", "ensure_catalog", "ensure_overrides"))
                mixed = common + sum(frames.get(name, 0) for name in ("build_list", "rh_manager_data_mix_get", "rh_manager_data_mix_count"))
                choices = common + sum(frames.get(name, 0) for name in ("build_choices", "rh_manager_data_option_get", "rh_manager_data_option_count", "theme_available", "system_preview"))
                self.assertLessEqual(mixed, 3072, f"empty mixed-page module frames: {mixed}")
                self.assertLessEqual(choices, 3072, f"empty choice-page module frames: {choices}")
                print(f"ARM manager {target}: empty mixed/choice frames {mixed}/{choices} bytes")


if __name__ == "__main__":
    unittest.main()
