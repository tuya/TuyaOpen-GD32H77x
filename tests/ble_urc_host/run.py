#!/usr/bin/env python3
"""Compile the actual gateway BLE emitter/macros against the actual host parser."""
from pathlib import Path
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
GATEWAY = ROOT / 'platform/GD32/gd32_os/MSDK/vw553_at_gateway'
AT = HERE.parents[1] / 'gd32h77x_os/components/at'
header = (GATEWAY / 'atcmd/atcmd.h').read_text()
macros = header[header.index('#define AT_RSP_START('):header.index('#define AT_RSP_ERR(')]
source = (GATEWAY / 'ble/atcmd_ble.c').read_text()
start = source.index('void at_ble_normal_trans_rx_callback(')
brace = source.index('{', start)
level = 1
end = brace + 1
while level:
    level += (source[end] == '{') - (source[end] == '}')
    end += 1
with tempfile.TemporaryDirectory(prefix='gd32h77x-ble-urc-') as tmp:
    folder = Path(tmp)
    (folder / 'gateway_ble_rx.inc').write_text(macros + '\n' + source[start:end] + '\n')
    exe = folder / 'test_ble_urc'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-I' + str(folder), '-I' + str(AT), str(HERE / 'test_ble_urc.c'),
                    str(AT / 'at_client.c'), str(AT / 'at_parser.c'),
                    str(AT / 'at_profile.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
