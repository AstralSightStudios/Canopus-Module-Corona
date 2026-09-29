#!/bin/sh
set -eu
[ "${RH_EXPERIMENTAL_FONT_RELOAD:-0}" = 0 ] || {
    printf 'Experimental fonts are not a release payload; use build.sh for the opt-in prototype.\n' >&2
    exit 1
}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CANOPUS=${CANOPUS_ROOT:-"$ROOT/../Canopus"}
if [ -z "${CANOPUS_ROOT:-}" ] && [ ! -d "$CANOPUS" ]; then
    CANOPUS="$ROOT/../Canopus-Private"
fi
export CANOPUS_ROOT="$CANOPUS"
TARGET_ID=${1:?usage: build-install-payload.sh target-id NEW-output-dir}
OUT=${2:?usage: build-install-payload.sh target-id NEW-output-dir}
[ "$#" -eq 2 ] || { printf 'Expected target-id and new output directory.\n' >&2; exit 1; }
case "$TARGET_ID" in
    xiaomi-band-11-4.100.139|xiaomi-band-11-4.100.155|xiaomi-band-10-pro-3.101.043) ;;
    *) printf 'Unsupported target: %s\n' "$TARGET_ID" >&2; exit 1 ;;
esac
[ ! -e "$OUT" ] && [ ! -L "$OUT" ] || { printf 'Output already exists; use a new directory: %s\n' "$OUT" >&2; exit 1; }
KEY=${MODULE_INSTALL_KEY:-"$CANOPUS/.canopus-local/module-installer-ed25519.pem"}
[ -f "$KEY" ] || { printf 'Set MODULE_INSTALL_KEY to the trusted Supervisor Ed25519 private PEM.\n' >&2; exit 1; }
command -v openssl >/dev/null
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
cp "$ROOT/build/resource-hook.elf" "$STAGE/resource-hook.elf"
python3 "$CANOPUS/scripts/build-module-installer-receipt.py" \
    --module "$STAGE/resource-hook.elf" \
    --module-id corona --version 3 --lifecycle 1 \
    --target-id "$TARGET_ID" \
    --firmware-sha256 "$FIRMWARE" \
    --private-key "$KEY" --output "$STAGE/receipt.bin"
openssl pkey -in "$KEY" -pubout -out "$STAGE/signer-public.pem"
python3 "$ROOT/scripts/verify-payload.py" "$STAGE" --target "$TARGET_ID" --public-key "$STAGE/signer-public.pem"
cp "$ROOT/examples/mappings.tsv" "$STAGE/mappings.tsv.example"
cp "$ROOT/docs/INSTALL.md" "$STAGE/INSTALL.md"
cp "$ROOT/docs/STARTUP_DIAGNOSTICS.md" "$STAGE/STARTUP_DIAGNOSTICS.md"
cp "$ROOT/scripts/verify-payload.py" "$STAGE/verify-payload.py"
python3 - "$STAGE" "$TARGET_ID" "$FIRMWARE" "$ROOT/Canopus.toml" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
import tomllib
p = Path(sys.argv[1])
manifest = tomllib.loads(Path(sys.argv[4]).read_text())
metadata = {
    "name": "Canopus-Module-Resource-Hook", "version": manifest['module']['version'],
    "project_id": manifest['module']['id'], "runtime_id": "corona",
    "receipt_module_version": 3, "receipt_format_version": 1,
    "target": sys.argv[2], "firmware_sha256": sys.argv[3], "format": "elf-cmi1",
    "lifecycle": "resident-after-activation", "physical_device": "NOT_PROBED",
    "automatic_ui_restart": False, "complete_cache_refresh": False,
}
(p / 'release.json').write_text(json.dumps(metadata, indent=2) + '\n')
files = sorted(f for f in p.iterdir() if f.is_file())
(p / 'SHA256SUMS').write_text(''.join(
    f'{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.name}\n' for f in files))
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
