#!/bin/sh
set -eu
TASK_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TASK_SDK=${CANOPUS_ROOT:-"$TASK_ROOT/../Canopus-Private"}
TASK_CC=${CC:-cc}
mkdir -p "$TASK_ROOT/build"
"$TASK_CC" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$TASK_ROOT/include" -I"$TASK_SDK/sdk/c" \
    "$TASK_ROOT/src/corona_archive.c" "$TASK_ROOT/tests/test_corona_archive.c" \
    -o "$TASK_ROOT/build/test_corona_archive"
python3 "$TASK_ROOT/tests/test_corona_archive.py" "$TASK_ROOT/build/test_corona_archive"
