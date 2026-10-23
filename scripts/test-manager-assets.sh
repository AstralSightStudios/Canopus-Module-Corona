#!/bin/sh
# Portable, offline asset conversion and production decoder regression suite.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
CC=${CC:-cc}
mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/generate-manager-assets.py" --check
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$ROOT/include" -I"$SDK/sdk/c" \
    "$ROOT/src/manager_assets.c" "$ROOT/tests/test_manager_assets.c" \
    -o "$ROOT/build/test_manager_assets"
python3 - "$ROOT" <<'PY'
import runpy
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
converter = runpy.run_path(str(root / 'scripts/generate-manager-assets.py'))
for fault in range(6):
    with tempfile.TemporaryDirectory(prefix='corona-assets-') as directory:
        subprocess.run([str(root / 'build/test_manager_assets'), directory, str(fault)], check=True)
        # A fresh module instance must reuse complete versioned files without
        # allocating a decode window or writing any bytes.
        subprocess.run([str(root / 'build/test_manager_assets'), directory, '6'], check=True)
        installed = Path(directory) / 'assets'
        for name, source in converter['ASSETS']:
            width, height, rgba = converter['read_png'](root / 'manager/src/common' / source)
            candidates = list(installed.glob(name + '-*.bin'))
            if name in ('reload', 'del'):
                candidates = [p for p in candidates if not p.name.startswith(name + '-wide-')]
            assert len(candidates) == 1, (name, candidates)
            image = candidates[0].read_bytes()
            converter['verify_image'](image, width, height, rgba)
            # Byte equality also catches header/palette drift in the C decoder.
            assert image == converter['to_lvgl'](width, height, rgba), name
        assert not list(installed.glob('*.part')), 'failed install leaked temporary files'
print('C-installed assets match all source PNG pixels and alpha; six fault/restart runs passed')
PY
