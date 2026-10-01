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
# Also test absent and explicitly disabled opt-in, without native leaf stubs.
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
    modes="default disabled"
    if [ "$target" != 1043 ]; then
        modes="default disabled experimental"
    fi
    for mode in $modes; do
        case "$mode" in
            default) FONT_DEFINE= ;;
            disabled) FONT_DEFINE=-DRH_EXPERIMENTAL_FONT_RELOAD=0 ;;
            experimental) FONT_DEFINE=-DRH_EXPERIMENTAL_FONT_RELOAD=1 ;;
        esac
        printf 'Host target .%s: %s font reload\n' "$target" "$mode"
        "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
            $TARGET_DEFINE $FONT_DEFINE -I"$ROOT/include" -I"$SDK/sdk/c" \
            "$ROOT/src/module.c" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
            "$SDK/runtime/control/canopus_control.c" "$ROOT/tests/test_module.c" \
            -o "$ROOT/build/test_module_${target}_${mode}"
        if [ "$mode" = experimental ]; then
            "$ROOT/build/test_module_${target}_${mode}" --experimental-fonts
        else
            "$ROOT/build/test_module_${target}_${mode}"
            "$ROOT/build/test_module_${target}_${mode}" --empty-startup
        fi
        "$ROOT/build/test_module_${target}_${mode}" --startup-diagnostics
        "$ROOT/build/test_module_${target}_${mode}" --snapshots
        "$ROOT/build/test_module_${target}_${mode}" --calendar
        for fault in open-fail write-fail short-write fd-zero; do
            RH_TEST_REGISTRATION="$fault" "$ROOT/build/test_module_${target}_${mode}" --startup-diagnostics
        done
        "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
            $TARGET_DEFINE $FONT_DEFINE -I"$ROOT/include" \
            "$ROOT/tests/test_font_reload.c" "$ROOT/src/resource_hook.c" \
            -o "$ROOT/build/test_font_reload_${target}_${mode}"
        "$ROOT/build/test_font_reload_${target}_${mode}"
    done
done
CC="$CC" python3 "$ROOT/tests/test_platform_io.py"
CC="$CC" python3 "$ROOT/tests/test_font_reload_targets.py"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" "$ROOT/tools/check_config.c" \
    -o "$ROOT/build/check-config"
python3 "$ROOT/tests/test_config_cli.py"
python3 "$ROOT/tests/test_target_receipts.py"
python3 "$ROOT/tests/firmware_support_test.py"
