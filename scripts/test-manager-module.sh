#!/bin/sh
# Actual module coordinator, native root and internal-state path under sanitizers.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
CC=${CC:-cc}
mkdir -p "$ROOT/build"
for target in 155 179 1043; do
    DEFINE=-DRH_TARGET_$target=1
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        "$DEFINE" -DRH_NATIVE_MANAGER=1 -DCANOPUS_HOST=1 \
        -I"$ROOT/include" -I"$SDK/sdk/c" -I"$SDK/runtime/hook" \
        "$ROOT/src/module.c" "$ROOT/src/manager_config_store.c" \
        "$ROOT/src/platform_fs.c" "$ROOT/src/platform_memory.c" \
        "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
        "$ROOT/src/launcher_background.c" \
        "$SDK/runtime/control/canopus_control.c" "$SDK/runtime/hook/canopus_hook.c" \
        "$ROOT/tests/test_module.c" -o "$ROOT/build/test_module_native_$target"
    "$ROOT/build/test_module_native_$target" --manager-internal
    "$ROOT/build/test_module_native_$target" --manager-reentrant
    "$ROOT/build/test_module_native_$target" --manager-generations
    "$ROOT/build/test_module_native_$target" --connect-rollback
done
