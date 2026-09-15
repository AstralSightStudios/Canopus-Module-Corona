#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CANOPUS=${CANOPUS_ROOT:-"$ROOT/../Canopus"}
TARGET_ID=${1:?usage: build-install-payload.sh target-id NEW-output-dir}
OUT=${2:?usage: build-install-payload.sh target-id NEW-output-dir}
[ "$#" -eq 2 ] || { printf 'Expected target-id and new output directory.\n' >&2; exit 1; }
[ "$TARGET_ID" = xiaomi-band-11-4.100.139 ] || { printf 'Unsupported target.\n' >&2; exit 1; }
[ ! -e "$OUT" ] && [ ! -L "$OUT" ] || { printf 'Output already exists; use a new directory: %s\n' "$OUT" >&2; exit 1; }
KEY=${MODULE_INSTALL_KEY:-"$CANOPUS/.canopus-local/module-installer-ed25519.pem"}
[ -f "$KEY" ] || { printf 'Set MODULE_INSTALL_KEY to the trusted Supervisor Ed25519 private PEM.\n' >&2; exit 1; }
command -v openssl >/dev/null
command -v python3 >/dev/null
sh "$ROOT/scripts/build.sh"
mkdir -p "$(dirname -- "$OUT")"
STAGE=$(mktemp -d "${OUT}.tmp.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT HUP INT TERM
cp "$ROOT/build/resource-hook.elf" "$STAGE/resource-hook.elf"
python3 "$CANOPUS/scripts/build-module-installer-receipt.py" \
    --module "$STAGE/resource-hook.elf" \
    --module-id resource_hook --version 3 --lifecycle 1 \
    --target-id "$TARGET_ID" \
    --firmware-sha256 31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74 \
    --private-key "$KEY" --output "$STAGE/receipt.bin"
openssl pkey -in "$KEY" -pubout -out "$STAGE/signer-public.pem"
python3 "$ROOT/scripts/verify-payload.py" "$STAGE" --public-key "$STAGE/signer-public.pem"
cp "$ROOT/examples/mappings.tsv" "$STAGE/mappings.tsv.example"
cp "$ROOT/docs/INSTALL.md" "$STAGE/INSTALL.md"
cp "$ROOT/scripts/verify-payload.py" "$STAGE/verify-payload.py"
python3 - "$STAGE" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
p = Path(sys.argv[1])
metadata = {
    "name": "Canopus-Module-Resource-Hook", "version": "0.3.0",
    "project_id": "org.canopus.resource-hook", "runtime_id": "resource_hook",
    "receipt_module_version": 3, "receipt_format_version": 1,
    "target": "xiaomi-band-11-4.100.139", "format": "elf-cmi1",
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
