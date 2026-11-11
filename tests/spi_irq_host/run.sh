#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../../.." && pwd)
adapter="$repo_root/platform/GD32H77X/tuyaos/tuyaos_adapter"
out=${TMPDIR:-/tmp}/gd32h77x_spi_irq_host_test

cc -std=c11 -Wall -Wextra -Werror -DGD32H77X_SPI_HOST_TEST \
    -I"$test_dir/fakes" \
    -I"$adapter/include/spi" \
    -I"$adapter/include/pinmux" \
    -I"$adapter/include/gpio" \
    -I"$adapter/include/utilities/include" \
    -I"$repo_root/src/common/include" \
    "$test_dir/test_spi_irq_host.c" \
    "$test_dir/fake_peripherals.c" \
    "$adapter/src/driver/tkl_spi.c" \
    -o "$out"

"$out"
