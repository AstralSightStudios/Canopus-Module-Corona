#!/bin/sh
# Portable config-pointer model only; no firmware power-loss guarantee.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
CC=${CC:-cc}
mkdir -p "$ROOT/build"
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$ROOT/include" -I"$SDK/sdk/c" \
    "$ROOT/src/manager_config_store.c" "$ROOT/tests/test_manager_config_store.c" \
    -o "$ROOT/build/test_manager_config_store"
"$ROOT/build/test_manager_config_store"
"$ROOT/build/test_manager_config_store" startup
"$ROOT/build/test_manager_config_store" root-no-slash
for fault in 0 1 2 3 4 5 6 7 8 9 10 11 12 embedded-nul oversized-read; do
    "$ROOT/build/test_manager_config_store" errors "$fault"
done
for fault in malformed read open close; do
    "$ROOT/build/test_manager_config_store" repair "$fault"
done
