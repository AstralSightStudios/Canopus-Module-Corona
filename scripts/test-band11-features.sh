#!/bin/sh
# Opt-in exact-AP/model suite. Requires capstone, unicorn and pyelftools.
# Does not access a device or modify the external SDK.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYTHON=${PYTHON:-python3}
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus"}
if [ -z "${CANOPUS_ROOT:-}" ] && [ ! -d "$SDK" ]; then
    SDK="$ROOT/../Canopus-Private"
fi
export CANOPUS_ROOT="$SDK"
"$PYTHON" -c 'import capstone, unicorn, elftools' || {
    printf 'Use PYTHON with capstone, unicorn and pyelftools installed.\n' >&2
    exit 1
}
# Missing/wrong APs must fail this explicit runner, never silently skip coverage.
for target in 139 177; do
    if [ "$target" = 139 ]; then
        AP=${RH_AP_139:-"$SDK/fwbins/xiaomi-band-11-4.100.139/vela_ap.bin"}
    else
        AP=${RH_AP_177:-"$SDK/fwbins/xiaomi-band-11-4.100.177/vela_ap.bin"}
    fi
    [ -f "$AP" ] || { printf 'Missing exact .%s AP: %s\n' "$target" "$AP" >&2; exit 1; }
    RH_CALENDAR_TARGET="$target" RH_CALENDAR_FIRMWARE="$AP" \
        "$PYTHON" "$ROOT/tests/test_calendar_native.py"
    RH_BACKGROUND_TARGET="$target" "$PYTHON" "$ROOT/tests/test_launcher_background_native.py"
    RH_BACKGROUND_TARGET="$target" "$PYTHON" "$ROOT/tests/test_blur_background_native.py"
done
"$PYTHON" "$ROOT/tests/test_band11_feature_ports.py"
printf 'Band11 exact-AP and model tests passed; device/GPU behavior remains NOT_PROBED.\n'
