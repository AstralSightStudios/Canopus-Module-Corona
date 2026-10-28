#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CANOPUS=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
export CANOPUS_ROOT="$CANOPUS"
TARGET_ID=${1:?usage: build-install-payload.sh target-id NEW-output-dir}
OUT=${2:?usage: build-install-payload.sh target-id NEW-output-dir}
[ "$#" -eq 2 ] || { printf 'Expected target-id and new output directory.\n' >&2; exit 1; }
case "$TARGET_ID" in
    xiaomi-band-11-4.100.179|xiaomi-band-11-4.100.177|xiaomi-band-11-4.100.139|xiaomi-band-11-4.100.155|xiaomi-band-10-pro-3.101.043|redmi-watch-6-3.100.043) ;;
    *) printf 'Unsupported target: %s\n' "$TARGET_ID" >&2; exit 1 ;;
esac
[ ! -e "$OUT" ] && [ ! -L "$OUT" ] || { printf 'Output already exists; use a new directory: %s\n' "$OUT" >&2; exit 1; }
if [ -z "${CANOPUS_CLI:-}" ] && [ ! -x "$CANOPUS/target/debug/canopus" ] && \
   [ -x "$ROOT/build/canopus-cli/debug/canopus" ]; then
    CANOPUS_CLI="$ROOT/build/canopus-cli/debug/canopus"
fi
CANOPUS_CLI=${CANOPUS_CLI:-"$CANOPUS/target/debug/canopus"}
export CANOPUS_CLI
command -v python3 >/dev/null
FIRMWARE=$(python3 - "$ROOT" "$TARGET_ID" <<'PY'
from pathlib import Path
import runpy
import sys
print(runpy.run_path(str(Path(sys.argv[1]) / 'scripts/verify-payload.py'))['TARGETS'][sys.argv[2]])
PY
)
sh "$ROOT/scripts/build.sh" "$TARGET_ID"
mkdir -p "$(dirname -- "$OUT")"
STAGE=$(mktemp -d "${OUT}.tmp.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT HUP INT TERM
# Package only this invocation's freshly built exact-target ELF. The CLI's
# package command selects every target in its manifest, so use an isolated
# project with the same identity and a narrowed target list.
python3 - "$ROOT" "$STAGE" "$TARGET_ID" <<'PY'
from pathlib import Path
import os
import re
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
root, stage = map(Path, sys.argv[1:3])
target = sys.argv[3]
manifest_text = (root / 'Canopus.toml').read_text()
manifest = tomllib.loads(manifest_text)
module = manifest['module']
cli = os.environ['CANOPUS_CLI']
with tempfile.TemporaryDirectory(prefix='canopus-project-', dir=stage) as temporary:
    project = Path(temporary)
    selected, count = re.subn(r'(?ms)^\[targets\][^\n]*\n.*?(?=^\[|\Z)',
        '[targets]\ninclude = ["' + target + '"]\n', manifest_text)
    if count != 1:
        raise ValueError('expected one [targets] table in Canopus.toml')
    (project / 'Canopus.toml').write_text(selected)
    runpy.run_path(str(root / 'scripts/build_options.py'))['stage_resources'](
        root, project, manifest, [target])
    artifact = project / 'build' / target / (module['id'] + '.elf')
    artifact.parent.mkdir(parents=True)
    shutil.copyfile(root / 'build' / target / artifact.name, artifact)
    subprocess.run([cli, 'verify', str(artifact), '--target', target,
                    '--targets-dir', str(Path(os.environ['CANOPUS_ROOT']) / 'targets')], check=True)
    package = stage / (module['id'] + '-' + module['version'] + '.canopus')
    subprocess.run([cli, 'package', '--manifest', str(project / 'Canopus.toml'),
                    '--output', str(package)], check=True)
    with tarfile.open(package) as archive:
        for member, destination in ((f'artifacts/{target}/module.elf', 'resource-hook.elf'),
                                    (f'receipts/{target}.cmi', 'receipt.bin')):
            with archive.extractfile(member) as stream:
                (stage / destination).write_bytes(stream.read())
    if (stage / 'resource-hook.elf').read_bytes() != artifact.read_bytes():
        raise ValueError('package artifact differs from the verified ELF')
PY
python3 "$ROOT/scripts/verify-payload.py" "$STAGE" --target "$TARGET_ID"
cp "$ROOT/examples/mappings.tsv" "$STAGE/mappings.tsv.example"
cp "$ROOT/docs/INSTALL.md" "$STAGE/INSTALL.md"
cp "$ROOT/docs/STARTUP_DIAGNOSTICS.md" "$STAGE/STARTUP_DIAGNOSTICS.md"
cp "$ROOT/docs/FONT_RELOAD.md" "$STAGE/FONT_RELOAD.md"
mkdir "$STAGE/evidence"
for evidence in "$ROOT/targets/$TARGET_ID"/*; do
    case "$evidence" in
        *.json|*.md) cp "$evidence" "$STAGE/evidence/" ;;
    esac
done
cp "$ROOT/scripts/verify-payload.py" "$STAGE/verify-payload.py"
python3 - "$STAGE" "$TARGET_ID" "$FIRMWARE" "$ROOT/Canopus.toml" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import runpy
import sys
import tomllib
p = Path(sys.argv[1])
manifest_path = Path(sys.argv[4])
manifest = tomllib.loads(manifest_path.read_text())
options = runpy.run_path(str(manifest_path.parent / 'scripts/build_options.py'))
metadata = {
    **options['feature_metadata']([sys.argv[2]]),
    **options['background_metadata']([sys.argv[2]],
        options['build_options']([sys.argv[2]], os.environ)),
    "name": "Canopus-Module-Resource-Hook", "version": manifest['module']['version'],
    "project_id": manifest['module']['id'], "runtime_id": "corona",
    "receipt_module_version": manifest['module']['version_code'], "receipt_format_version": 2,
    "target": sys.argv[2], "firmware_sha256": sys.argv[3], "format": "elf-cmi2",
    "lifecycle": "resident-after-activation", "physical_device": "NOT_PROBED",
    "module_build_id": "resource-hook-0.3.0",
    "gpu_recovery": "UNSUPPORTED", "framework_restart_safety": "UNSUPPORTED",
    "automatic_ui_restart": False, "complete_cache_refresh": False,
}
(p / 'release.json').write_text(json.dumps(metadata, indent=2) + '\n')
files = sorted(f for f in p.rglob("*") if f.is_file())
(p / 'SHA256SUMS').write_text(''.join(
    f'{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.relative_to(p).as_posix()}\n' for f in files))
PY
# Rename only a complete, verified payload; never merge it into an older output.
python3 - "$STAGE" "$OUT" <<'PY'
from pathlib import Path
import sys
source, target = map(Path, sys.argv[1:])
if target.exists() or target.is_symlink():
    raise SystemExit('Output appeared during build; refusing to replace it')
source.rename(target)
PY
printf 'Delivery payload: %s\n' "$OUT"
