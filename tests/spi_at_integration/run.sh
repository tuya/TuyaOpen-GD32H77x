#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../../.." && pwd)
out=${TMPDIR:-/tmp}/gd32h77x_spi_at_integration

for cs_only in 0 1; do
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DAT_SPI_CS_ONLY=$cs_only -DVW553_GATEWAY_SPI_CS_ONLY=$cs_only \
    -I"$repo_root/platform/GD32H77X/gd32h77x_os/components/at" \
    -I"$repo_root/platform/GD32/gd32_os/MSDK/vw553_at_gateway" \
    "$test_dir/test_spi_at_integration.c" \
    "$repo_root/platform/GD32H77X/gd32h77x_os/components/at/at_spi_protocol.c" \
    "$repo_root/platform/GD32/gd32_os/MSDK/vw553_at_gateway/gateway_spi_protocol.c" \
    -o "$out"

ASAN_OPTIONS=detect_leaks=0 "$out"
done

rx_out=${TMPDIR:-/tmp}/gd32h77x_vw553_transport_rx_test
for cs_only in 0 1; do
for desk_pins in 0 1; do
if [ "$cs_only" = 0 ] && [ "$desk_pins" = 1 ]; then continue; fi
cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DVW553_GATEWAY_SPI_CS_ONLY=$cs_only \
    -DVW553_GATEWAY_SPI_DESK_BUDDY_PINS=$desk_pins \
    -ffunction-sections -fdata-sections -Wno-pointer-to-int-cast \
    -I"$test_dir/fakes" \
    -I"$repo_root/platform/GD32/gd32_os/MSDK/vw553_at_gateway" \
    "$test_dir/test_vw553_transport_rx.c" \
    "$repo_root/platform/GD32/gd32_os/MSDK/vw553_at_gateway/gateway_spi_protocol.c" \
    -Wl,--gc-sections -o "$rx_out"
ASAN_OPTIONS=detect_leaks=0 "$rx_out"
done
done
