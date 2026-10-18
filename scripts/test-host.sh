#!/bin/sh
# Host models only: no device/GPU acceptance or private-checkout writes.
set -eu
# Exact .177 pack/fingerprint checks are also available via tests/test_target_177.py;
# they require the reviewed private pack and AP, unlike portable host suites.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
CC=${CC:-cc}
if [ -z "${CANOPUS_CLI:-}" ] && [ ! -x "$SDK/target/debug/canopus" ] && \
    [ -x "$ROOT/build/canopus-cli/debug/canopus" ]; then
    CANOPUS_CLI="$ROOT/build/canopus-cli/debug/canopus"
    export CANOPUS_CLI
fi
export CANOPUS_ROOT="$SDK"
[ -f "$SDK/sdk/c/canopus_abi.h" ] || { printf 'Set CANOPUS_ROOT to the Canopus checkout.\n' >&2; exit 1; }
mkdir -p "$ROOT/build"
for test in hook boundaries; do
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$ROOT/include" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
        "$ROOT/tests/test_$test.c" -o "$ROOT/build/test_$test"
    "$ROOT/build/test_$test"
done
for target in 139 1043; do
    DEFINE=
    [ "$target" != 1043 ] || DEFINE=-DRH_TARGET_1043=1
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        $DEFINE -I"$ROOT/include" "$ROOT/src/launcher_background.c" \
        "$ROOT/tests/test_launcher_background.c" -o "$ROOT/build/test_launcher_background_$target"
    "$ROOT/build/test_launcher_background_$target"
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        $DEFINE -I"$ROOT/include" "$ROOT/tests/test_feature_helpers.c" \
        "$ROOT/src/calendar_config.c" \
        -o "$ROOT/build/test_feature_helpers_$target"
    "$ROOT/build/test_feature_helpers_$target"
done
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$ROOT/include" "$ROOT/src/calendar_config.c" "$ROOT/src/calendar_icon.c" \
    "$ROOT/tests/test_calendar.c" -o "$ROOT/build/test_calendar"
"$ROOT/build/test_calendar"
CC="$CC" python3 "$ROOT/tests/test_launcher_background_native.py"
CC="$CC" python3 "$ROOT/tests/test_blur_background_native.py"
python3 "$ROOT/tests/test_p67_launcher_native.py"
CC="$CC" python3 "$ROOT/tests/test_p67_blur_provider.py"
CC="$CC" python3 "$ROOT/tests/test_p67_battery.py"
# Production native blur must link without a diagnostic getter; the opt-in
# variant separately exercises the existing bounded diagnostic writer.
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DRH_TARGET_1043=1 \
    -DRH_P67_BLUR_DIAGNOSTICS=1 -I"$ROOT/include" -I"$SDK/sdk/c" \
    "$ROOT/src/module.c" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
    "$ROOT/src/launcher_background.c" "$SDK/runtime/control/canopus_control.c" \
    "$ROOT/tests/test_module.c" -o "$ROOT/build/test_module_1043_diagnostics"
"$ROOT/build/test_module_1043_diagnostics" --blur-diagnostics
# Compile the actual shared transaction for each exact address selection.
# Fonts and backgrounds are ordinary runtime features on all four targets.
for target in 139 155 177 1043; do
    if [ "$target" = 177 ]; then
        TARGET_DEFINE=-DRH_TARGET_177=1
    elif [ "$target" = 1043 ]; then
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
        "$ROOT/src/launcher_background.c" \
        "$SDK/runtime/control/canopus_control.c" "$ROOT/tests/test_module.c" \
        -o "$ROOT/build/test_module_${target}"
    "$ROOT/build/test_module_${target}" --time-services
    "$ROOT/build/test_module_${target}" --time-activation-failure 50
    "$ROOT/build/test_module_${target}" --time-activation-failure 1000
    "$ROOT/build/test_module_${target}" --launcher-background
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
    "$ROOT/build/test_module_${target}" --calendar-alpha-hook
    "$ROOT/build/test_module_${target}" --calendar-coordinator-guards
    "$ROOT/build/test_module_${target}" --calendar-initial-invalid
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
python3 "$ROOT/tests/test_build_delivery.py"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" "$ROOT/tools/check_config.c" \
    -o "$ROOT/build/check-config"
python3 "$ROOT/tests/test_config_cli.py"
python3 "$ROOT/tests/test_target_receipts.py"
python3 "$ROOT/tests/test_quickapp_allowlist.py"
python3 "$ROOT/tests/firmware_support_test.py"

# Exact APs and emulator dependencies are intentionally optional for host users.
if [ "${RH_TEST_BAND11_FEATURES:-0}" = 1 ]; then
    sh "$ROOT/scripts/test-band11-features.sh"
fi
