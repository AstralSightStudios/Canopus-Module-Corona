#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
[ "$#" -le 1 ] || { printf 'usage: build.sh [target-id]\n' >&2; exit 1; }
TARGET_ID=${1:-xiaomi-band-11-4.100.139}
case "$TARGET_ID" in
    xiaomi-band-11-4.100.139|xiaomi-band-11-4.100.177|xiaomi-band-11-4.100.155|xiaomi-band-10-pro-3.101.043) ;;
    *) printf 'Unsupported target: %s\n' "$TARGET_ID" >&2; exit 1 ;;
esac
# One complete product per exact target. Refuse stale product selectors,
# including explicit zero, rather than silently changing their old semantics.
for legacy in RH_P67_LIVE_LAUNCHER RH_P67_LIVE_BACKGROUNDS RH_P67_BLUR_NATIVE; do
    eval 'value=${'"$legacy"'+set}'
    [ -z "$value" ] || { printf 'Retired build flag %s; remove it. P67 backgrounds and native blur are now default.\n' "$legacy" >&2; exit 1; }
done
if [ "${RH_P67_BLUR_DIAGNOSTICS:-0}" = 1 ]; then
    [ "$TARGET_ID" = xiaomi-band-10-pro-3.101.043 ] || { printf 'Blur diagnostics requires exact .043 target.\n' >&2; exit 1; }
fi
CC=${CC:-cc}
CLANG=${CLANG:-clang}
LD_LLD=${LD_LLD:-ld.lld}
if [ -z "${CANOPUS_CLI:-}" ] && [ ! -x "$SDK/target/debug/canopus" ] && \
    [ -x "$ROOT/build/canopus-cli/debug/canopus" ]; then
    CANOPUS_CLI="$ROOT/build/canopus-cli/debug/canopus"
fi
CANOPUS_CLI=${CANOPUS_CLI:-"$SDK/target/debug/canopus"}
for tool in "$CC" "$CLANG" "$LD_LLD"; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'Missing tool: %s\n' "$tool" >&2; exit 1; }
done
[ -f "$SDK/sdk/c/canopus_abi.h" ] || { printf 'Set CANOPUS_ROOT to the Canopus checkout.\n' >&2; exit 1; }
[ -x "$CANOPUS_CLI" ] || { printf 'Build the Canopus CLI first: cargo build -p canopus-cli (in Canopus).\n' >&2; exit 1; }
# Check the selected framework pack against our pinned AP identity before compiling.
python3 - "$ROOT" "$SDK" "$TARGET_ID" <<'PY'
from pathlib import Path
import re
import runpy
import sys
import tomllib
root, sdk, target = sys.argv[1:]
firmware = runpy.run_path(str(Path(root) / 'scripts/verify-payload.py'))['TARGETS'][target]
pack = Path(sdk) / 'targets' / target
metadata = tomllib.loads((pack / 'target.toml').read_text())
header = (pack / 'generated/canopus_target_config.h').read_text()
identity = re.search(r'^#define CANOPUS_TARGET_ID "([^"]+)"$', header, re.M)
fingerprint = re.search(r'#define CANOPUS_SUP_FIRMWARE_SHA256_BYTES\s+\\\s*\{([^}]+)\}', header)
header_hash = ''.join(re.findall(r'0x([0-9a-fA-F]{2})\b', fingerprint[1])).lower() if fingerprint else None
if (metadata.get('target_id') != target or metadata.get('firmware_sha256') != firmware
        or not identity or identity[1] != target or header_hash != firmware):
    raise SystemExit('Framework target/firmware identity mismatch')
PY
CANOPUS_ROOT="$SDK" CANOPUS_CLI="$CANOPUS_CLI" CC="$CC" sh "$ROOT/scripts/test-host.sh"
CANOPUS_ROOT="$SDK" CC="$ROOT/scripts/canopus-cc.py" CLANG="$CLANG" \
    LD="$ROOT/scripts/canopus-ld.sh" LD_LLD="$LD_LLD" \
    "$CANOPUS_CLI" build --manifest "$ROOT/Canopus.toml" --target "$TARGET_ID"
cp "$ROOT/build/$TARGET_ID/corona.elf" "$ROOT/build/resource-hook.elf"
printf 'Built resource-hook.elf for %s. Static/build verification only; this artifact and .177 device behavior: NOT_PROBED.\n' "$TARGET_ID"
printf 'GPU recovery and framework restart safety remain unsupported.\n'
