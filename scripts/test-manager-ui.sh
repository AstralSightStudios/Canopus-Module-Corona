#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
CC=${CC:-cc}
mkdir -p "$ROOT/build"
for variant in production probe; do
    define=
    [ "$variant" != probe ] || define=-DRH_MANAGER_UI_TEST=1
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $define \
        -I"$ROOT/include" -I"$SDK/sdk/c" \
        "$ROOT/src/manager_ui.c" "$SDK/runtime/ui/canopus_ui.c" \
        "$ROOT/tests/test_manager_ui.c" -o "$ROOT/build/test_manager_ui_$variant"
    "$ROOT/build/test_manager_ui_$variant"
    "$ROOT/build/test_manager_ui_$variant" --wide
    "$ROOT/build/test_manager_ui_$variant" --p65
done
