# GinoPi 板载 GD32VW553：UART ISP 烧录

本文记录已在 GinoPi（GD32H77D + 板载 GD32VW553）验证的从片烧录流程。VW553 是无线协处理器；它的镜像、启动脚和烧录工具与 H77 主控完全独立。

## 先区分三组串口参数

| 用途 | 端口/链路 | 参数 |
| --- | --- | --- |
| VW553 ROM ISP 初始握手 | `/dev/ttyUSB0` | 57600, 8E1 |
| SRAM loader 上传后的下载 | `/dev/ttyUSB0` | 115200, 8N1；实际写镜像为 921600, 8N1 |
| 正常运行时 H77 ↔ VW553 AT | H77 UART4（PB13/PB12） | 115200, 8N1 |
| 正常运行时 VW553 原生日志/ISP | `/dev/ttyUSB0`（PA6/PA7） | 921600, 8N1 |

`921600` 是 ISP loader 与原生日志速率，**不是** H77 与模块 AT 的速率；AT 固件应保持 115200。

## 前置条件

- H77 运行带 `diag_vw553_reset` CLI 的 TuyaOpen 固件，且 H77 的 1 号拨码在 `OFF/L` 运行模式。
- VW553 的 BOOT0 可由用户拉高/拉低。该板没有独立的 VW553 复位键，H77 的 PC5 控制模块 NRST。
- 串口映射为 `/dev/ttyUSB0`（VW553）与 `/dev/ttyUSB1`（H77 控制台）；烧录前确认没有其他进程占用这两个端口。
- 待烧录镜像为 VW553 SDK 根目录的 `scripts/images/image-all.bin`；不要误用仅包含应用的 `MSDK.bin`。

## 构建镜像

在 VW553 SDK 根目录执行（工具链路径以本机安装位置为准）：

```sh
PATH=/home/share/samba/TuyaOpen/platform/tools/nuclei_riscv_newlibc_prebuilt_linux64_2022.04/gcc/bin:$PATH \
  cmake --build cmake_build --target MSDK --parallel 4
```

构建成功后使用 `scripts/images/image-all.bin`。它包含 MBL 与应用，烧录起点固定为 `0x08000000`。

## 实板烧录步骤

1. 断开手机 BLE 连接，停止任何串口监听程序。
2. 将 **VW553 BOOT0 拉高**。
3. 保持 H77 运行，向 H77 控制台发送 `diag_vw553_reset isp`。这会由 PC5 拉复位，使 VW553 在 BOOT0 高时进入 ROM ISP；该命令也会暂时阻断 H77 的 AT 自动恢复。
4. 执行下列 Python 调用。工具会自动完成 ROM 同步、SRAM loader 上传、范围擦除、写入和校验：

```sh
python3 -c '
import importlib.util
from pathlib import Path
tool = Path("/home/share/samba/TuyaOpen/docs/handoffs/gd32h77x-20261016/restore-tools/tools/gd32_isp_flash.py")
spec = importlib.util.spec_from_file_location("gd", tool)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
m.TOOLS_DIR = Path("/tmp")
m.flash_firmware(device="GD32VW553H", port="/dev/ttyUSB0", baud=921600,
                 start="0x08000000",
                 binfile="/absolute/path/to/scripts/images/image-all.bin")
'
```

5. 成功后将 **BOOT0 拉低**。
6. 按一次主板复位键（H77 仍为 `OFF/L`），或断电重上电。H77 必须重新启动，才会重新建立 AT 会话并重新执行 BLE 初始化/广播。

## 验证与故障处理

- 正常启动后，H77 控制台应能完成 `AT+BLEENABLE`、广告数据设置和 `AT+BLEADVSTART`；手机可重新扫描到广播。
- 若烧录后 H77 不广播，优先检查 BOOT0 是否已拉低；随后复位 **H77 与 VW553**，不要只等待模块自行恢复。
- 若 AT 连续超时或出现 `transport poison`，通常表示模块仍在 ISP、未复位启动，或 AT 线速误设为 921600。先恢复 BOOT0 低、主板复位，并以 115200 检查 AT。
- 写入工具只擦除/写入 `image-all.bin` 覆盖的镜像范围；不要扩展到整个 Flash，避免破坏末尾的校准或设备数据。
- 553 烧录失败时，先重新确认 BOOT0 高并再次执行 `diag_vw553_reset isp`；不要把 H77 DFU 拨码切到 `ON/H`，那是主控下载模式，与 VW553 ISP 无关。
