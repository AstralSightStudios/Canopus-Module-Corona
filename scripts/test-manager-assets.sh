#!/bin/sh
# Portable, offline resource generation and production resolver regression suite.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
CC=${CC:-cc}
mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/generate-manager-assets.py"
python3 "$ROOT/scripts/generate-manager-assets.py" --check
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$ROOT/include" -I"$SDK/sdk/c" \
    "$ROOT/src/manager_assets.c" "$ROOT/tests/test_manager_assets.c" \
    -o "$ROOT/build/test_manager_assets"
python3 - "$ROOT" <<'PY'
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
generator = runpy.run_path(str(root / 'scripts/generate-manager-assets.py'))
res_dir = root / 'build/manager-resources'
files, _, _ = generator['build_resources']()

# The shipped .bin resources must be byte-identical to the source PNG pixels.
for name, source in generator['ASSETS']:
    width, height, rgba = generator['read_png'](root / 'assets/manager' / source)
    image = (res_dir / f'{name}.bin').read_bytes()
    assert image == generator['to_lvgl'](width, height, rgba), name
    generator['verify_image'](image, width, height, rgba)

# Fresh data root per fault; the C harness mutates one resource itself.
for fault in range(6):
    with tempfile.TemporaryDirectory(prefix='corona-assets-') as directory:
        dest = Path(directory)
        for filename, image in files.items():
            (dest / filename).write_bytes(image)
        subprocess.run([str(root / 'build/test_manager_assets'), directory, str(fault)], check=True)

print('Extracted resources match all source PNG pixels; six resolution/fault runs passed')
PY
