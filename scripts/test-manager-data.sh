#!/bin/sh
set -eu
TASK_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TASK_SDK=${CANOPUS_ROOT:-"$TASK_ROOT/../Canopus-Private"}
TASK_CC=${CC:-cc}
mkdir -p "$TASK_ROOT/build"
for target in 155 1043; do
    "$TASK_CC" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
        -fsanitize=address,undefined -g -DRH_TARGET_$target=1 \
        -I"$TASK_ROOT/include" -I"$TASK_SDK/sdk/c" \
        "$TASK_ROOT/src/manager_data.c" "$TASK_ROOT/tests/test_manager_data.c" \
        -o "$TASK_ROOT/build/test_manager_data_$target"
    "$TASK_ROOT/build/test_manager_data_$target"
done
