#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus"}
CC=${CC:-cc}
CLANG=${CLANG:-clang}
LD_LLD=${LD_LLD:-ld.lld}
CANOPUS_CLI=${CANOPUS_CLI:-"$SDK/target/debug/canopus"}
for tool in "$CC" "$CLANG" "$LD_LLD"; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'Missing tool: %s\n' "$tool" >&2; exit 1; }
done
[ -f "$SDK/sdk/c/canopus_abi.h" ] || { printf 'Set CANOPUS_ROOT to the Canopus checkout.\n' >&2; exit 1; }
[ -x "$CANOPUS_CLI" ] || { printf 'Build the Canopus CLI first: cargo build -p canopus-cli (in Canopus).\n' >&2; exit 1; }
mkdir -p "$ROOT/build"
for test in hook boundaries; do
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$ROOT/include" "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
        "$ROOT/tests/test_$test.c" -o "$ROOT/build/test_$test"
    "$ROOT/build/test_$test"
done
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$ROOT/include" -I"$SDK/sdk/c" "$ROOT/src/module.c" \
    "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" \
    "$SDK/runtime/control/canopus_control.c" "$ROOT/tests/test_module.c" -o "$ROOT/build/test_module"
"$ROOT/build/test_module"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/src/resource_hook.c" "$ROOT/src/config.c" "$ROOT/tools/check_config.c" \
    -o "$ROOT/build/check-config"
python3 "$ROOT/tests/test_config_cli.py"
for name in module resource_hook config platform_band11; do
    "$CLANG" -Wall -Wextra -Werror --target=arm-none-eabi -mcpu=cortex-m33 -mthumb \
        -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables \
        -Os -I"$SDK/sdk/c" -I"$SDK/manager/target/band11" \
        -I"$SDK/targets/xiaomi-band-11-4.100.139/generated" -I"$ROOT/include" \
        -c "$ROOT/src/$name.c" -o "$ROOT/build/$name.o"
done
"$CLANG" -Wall -Wextra -Werror --target=arm-none-eabi -mcpu=cortex-m33 -mthumb \
    -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables \
    -Os -I"$SDK/sdk/c" -c "$SDK/runtime/control/canopus_control.c" -o "$ROOT/build/control.o"
"$LD_LLD" -r -T "$SDK/scripts/canopus_supervisor_sections.ld" \
    "$ROOT/build/module.o" "$ROOT/build/resource_hook.o" "$ROOT/build/config.o" \
    "$ROOT/build/platform_band11.o" "$ROOT/build/control.o" -o "$ROOT/build/resource-hook.elf"
"$CANOPUS_CLI" verify "$ROOT/build/resource-hook.elf" \
    --target xiaomi-band-11-4.100.139 --targets-dir "$SDK/targets"
printf 'Built resource-hook.elf. Static validation only; physical-device acceptance remains required.\n'
