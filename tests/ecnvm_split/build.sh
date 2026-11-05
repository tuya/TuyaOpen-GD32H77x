#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
tool_bin="/opt/gcc-arm-none-eabi-10.3-2021.10/bin"
out_dir="${repo_root}/.cache/gd32h779v_start/ecnvm-split"
gd_dir="${repo_root}/platform/GD32H77X/gd32h77x_os/gd32"
target_dir="${repo_root}/platform/GD32H77X/gd32h77x_os/target"
cmsis="${gd_dir}/CMSIS/GD/GD32H77x_78x"
periph="${gd_dir}/GD32H77x_78x_standard_peripheral"
config_dir="${out_dir}/platform_config"
src_dir="${repo_root}/platform/GD32H77X/tests/ecnvm_split"
cc="${tool_bin}/arm-none-eabi-gcc"
objcopy="${tool_bin}/arm-none-eabi-objcopy"

mkdir -p "${out_dir}"
"${repo_root}/.venv/bin/python" "${repo_root}/platform/GD32H77X/platform_config.py" \
    --platform-root "${repo_root}/platform/GD32H77X" \
    --boards-root "${repo_root}/boards" \
    --chip GD32H779VW --board GD32H779V_START \
    --out-dir "${config_dir}"
common=(
    -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
    -ffunction-sections -fdata-sections -fno-common -Os -g3
    -DGD32H779XW -DUSE_STDPERIPH_DRIVER -DHXTAL_VALUE=25000000U
    -I"${config_dir}/include" -I"${target_dir}" -I"${cmsis}/Include" -I"${cmsis}/Source" -I"${gd_dir}/CMSIS" -I"${periph}/Include"
)

"${cc}" "${common[@]}" -c "${src_dir}/ecnvm_split.c" -o "${out_dir}/ecnvm_split.o"
"${cc}" "${common[@]}" -c "${src_dir}/libc_stubs.c" -o "${out_dir}/libc_stubs.o"
"${cc}" "${common[@]}" -c "${src_dir}/split_startup.S" -o "${out_dir}/startup.o"
"${cc}" "${common[@]}" -c "${cmsis}/Source/GCC/startup_gd32h77x_78x.S" -o "${out_dir}/vendor_vectors.o"
"${cc}" "${common[@]}" -c "${cmsis}/Source/system_gd32h77x_78x.c" -o "${out_dir}/system.o"
for driver in rcu gpio usart misc; do
    "${cc}" "${common[@]}" -c "${periph}/Source/gd32h77x_78x_${driver}.c" -o "${out_dir}/${driver}.o"
done

"${cc}" -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard \
    -nostartfiles --specs=nosys.specs -Wl,--gc-sections \
    -Wl,-Map,"${out_dir}/ecnvm_split.map" \
    -T"${src_dir}/linker.ld" \
    "${out_dir}/ecnvm_split.o" "${out_dir}/libc_stubs.o" "${out_dir}/startup.o" "${out_dir}/vendor_vectors.o" "${out_dir}/system.o" \
    "${out_dir}/rcu.o" "${out_dir}/gpio.o" "${out_dir}/usart.o" "${out_dir}/misc.o" \
    -lc -lgcc -o "${out_dir}/ecnvm_split.elf"

"${objcopy}" -O binary -j .vectors -j .text -j .ARM.extab \
    -j .ARM.exidx -j .preinit_array -j .init_array -j .fini_array -j .data \
    "${out_dir}/ecnvm_split.elf" "${out_dir}/cnvm.bin"
"${objcopy}" -O binary -j .ecnvm_text -j .ecnvm_rodata \
    "${out_dir}/ecnvm_split.elf" "${out_dir}/ecnvm.bin"
"${objcopy}" -O ihex "${out_dir}/ecnvm_split.elf" "${out_dir}/ecnvm_split.hex"

"${tool_bin}/arm-none-eabi-size" -A "${out_dir}/ecnvm_split.elf"
"${tool_bin}/arm-none-eabi-readelf" -h -S -s "${out_dir}/ecnvm_split.elf" > "${out_dir}/readelf.txt"
"${tool_bin}/arm-none-eabi-objdump" -d -j .ecnvm_text "${out_dir}/ecnvm_split.elf" > "${out_dir}/ecnvm_disassembly.txt"
cnvm_bytes="$(wc -c < "${out_dir}/cnvm.bin")"
ecnvm_bytes="$(wc -c < "${out_dir}/ecnvm.bin")"
if (( cnvm_bytes > 65536 || ecnvm_bytes > 65536 )); then
    printf 'Image exceeds first-sector safety limit (CNVM=%s, ECNVM=%s bytes)\n' "${cnvm_bytes}" "${ecnvm_bytes}" >&2
    exit 1
fi
printf 'CNVM footprint: %s bytes at 0x08000000; ECNVM footprint: %s bytes at 0x08200000\n' "${cnvm_bytes}" "${ecnvm_bytes}"
printf 'Artifacts: %s\n' "${out_dir}"
