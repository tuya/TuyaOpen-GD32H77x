#!/bin/sh
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
test_dir="$repo_root/platform/GD32H77X/tests"
adapter="$repo_root/platform/GD32H77X/tuyaos/tuyaos_adapter"
out=${TMPDIR:-/tmp}/gd32h77x_pinmux_host_test
cc -std=c11 -Wall -Wextra -Werror \
    -I"$test_dir/pinmux_fakes" \
    -I"$adapter/include/pinmux" \
    -I"$adapter/include/gpio" \
    -I"$adapter/include/utilities/include" \
    -I"$test_dir/fixtures" \
    -I"$repo_root/src/common/include" \
    -I"$adapter/src/driver" \
    "$test_dir/test_tkl_pinmux_host.c" \
    "$adapter/src/driver/tkl_pinmux.c" \
    "$adapter/src/driver/tkl_gpio.c" \
    -o "$out"
"$out"
