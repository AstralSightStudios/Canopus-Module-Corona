#!/bin/sh
# Host models only: no device/GPU acceptance or private-checkout writes.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus"}
if [ -z "${CANOPUS_ROOT:-}" ] && [ ! -d "$SDK" ]; then
    SDK="$ROOT/../Canopus-Private"
fi
CC=${CC:-cc}
[ -f "$SDK/sdk/c/canopus_abi.h" ] || { printf 'Set CANOPUS_ROOT to the Canopus checkout.\n' >&2; exit 1; }
mkdir -p "$ROOT/build"
for test in hook boundaries; do
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$ROOT/include" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
        "$ROOT/tests/test_$test.c" -o "$ROOT/build/test_$test"
    "$ROOT/build/test_$test"
done
# Compile the actual shared transaction for each exact address selection.
# Fonts are part of the ordinary runtime; no opt-in or legacy stub matrix.
for target in 139 155 1043; do
    if [ "$target" = 1043 ]; then
        TARGET_DEFINE=-DRH_TARGET_1043=1
    elif [ "$target" = 155 ]; then
        TARGET_DEFINE=-DRH_TARGET_155=1
    else
        TARGET_DEFINE=
    fi
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        $TARGET_DEFINE -I"$ROOT/include" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
        "$ROOT/tests/test_compact.c" -o "$ROOT/build/test_compact_${target}"
    "$ROOT/build/test_compact_${target}"
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        $TARGET_DEFINE -I"$ROOT/include" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
        "$ROOT/tests/test_quickapp.c" -o "$ROOT/build/test_quickapp_${target}"
    "$ROOT/build/test_quickapp_${target}"
    printf 'Host target .%s: default font reload\n' "$target"
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        $TARGET_DEFINE -I"$ROOT/include" -I"$SDK/sdk/c" \
        "$ROOT/src/module.c" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
        "$SDK/runtime/control/canopus_control.c" "$ROOT/tests/test_module.c" \
        -o "$ROOT/build/test_module_${target}"
    "$ROOT/build/test_module_${target}" --font-reload
    "$ROOT/build/test_module_${target}"
    "$ROOT/build/test_module_${target}" --empty-startup
    "$ROOT/build/test_module_${target}" --startup-diagnostics
    "$ROOT/build/test_module_${target}" --control-quickapp-status
    "$ROOT/build/test_module_${target}" --control-reload-status
    for startup in missing empty comments valid; do
        "$ROOT/build/test_module_${target}" --control-status "$startup"
    done
    for fault in eacces open-io unknown-errno scratch-oom snapshot-oom \
                 materialize-oom materialized-map-oom read-io malformed outside duplicate oversized; do
        "$ROOT/build/test_module_${target}" --startup-fallback "$fault"
    done
    "$ROOT/build/test_module_${target}" --snapshots
    "$ROOT/build/test_module_${target}" --calendar
    "$ROOT/build/test_module_${target}" --quickapp
    for fault in open-fail write-fail short-write fd-zero; do
        RH_TEST_REGISTRATION="$fault" "$ROOT/build/test_module_${target}" --startup-diagnostics
    done
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        $TARGET_DEFINE -I"$ROOT/include" \
        "$ROOT/tests/test_font_reload.c" "$ROOT/src/resource_hook.c" \
        -o "$ROOT/build/test_font_reload_${target}"
    "$ROOT/build/test_font_reload_${target}"
done
CC="$CC" python3 "$ROOT/tests/test_platform_io.py"
CC="$CC" python3 "$ROOT/tests/test_quickapp_icon_native.py"
CC="$CC" python3 "$ROOT/tests/test_font_reload_targets.py"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" "$ROOT/tools/check_config.c" \
    -o "$ROOT/build/check-config"
python3 "$ROOT/tests/test_config_cli.py"
python3 "$ROOT/tests/test_target_receipts.py"
python3 "$ROOT/tests/test_quickapp_allowlist.py"
python3 "$ROOT/tests/firmware_support_test.py"
