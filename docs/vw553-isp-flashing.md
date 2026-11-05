# LCKFB GD32VW553：构建、UART ISP 烧录与启动验证

VW553 是独立无线 MCU。其固件构建和烧录与 H779/H77 主控二进制完全独立。
本文当前描述的 canonical 入口是 `platform/GD32/gd32_os/MSDK/vw553_at_gateway`。

如果 VW553 固件不需要修改，可直接使用 H77 平台仓库预置的稳定 UART 镜像，
不需要拉取 `platform/GD32` 源码仓库：

```text
platform/GD32H77X/wireless_fw/vw553/uart/vw553_at_gateway_uart_v0.5.2.bin
```

版本、SHA256、引脚和验收边界见
[VW553 wireless firmware](../wireless_fw/vw553/README.md)。

不修改 VW553 固件时，推荐使用 GigaDevice 官方独立工具
**GD32 Mass Production Tool**。已验证版本 `v1.0.5g`，包 SHA256 为
`33559c67b294e7867867ba1c6063d2a64c8bc0cd0922df9bb0bbd879d0633faa`。
该路径不需要拉取 `platform/GD32` 源码仓库。

只有需要修改 VW553 源码时才拉取 `platform/GD32`，修改
`gd32_os/MSDK/vw553_at_gateway` 后重新编译和验收，并直接使用自己构建的
`image-all.bin`。开发者不需要把修改后的固件放回 H77 仓库；H77 仓库中的稳定
bin 由平台维护者按发布节奏更新。

> **历史入口说明**
>
> 旧的 `/tmp/gd32vw553-diag`、`MSDK/projects/vw553_at_gateway`、CMake 外部
> `cmake_build` 目录、overlay/staging 和手工 `patch -p*` 顺序仅作为历史验证记录，
> 不是当前构建步骤。H77 的 `diag_vw553_reset isp` 也只适用于明确提供该诊断命令且
> 复位接线成立的设备，不能作为唯一或默认 ISP 入口。

## 串口和引脚

| 用途 | 连接 | 参数 |
|---|---|---|
| VW553 ROM ISP 初始握手 | 板载 Type-C / USART0，主机枚举为 `/dev/ttyUSB*` | 57600 8E1 |
| SRAM loader 上传后 | 同一 Type-C 口 | 115200 8N1 |
| 镜像下载 | 同一 Type-C 口 | 默认 2,000,000 8N1 |
| VW553 正常运行时 LOG UART | UART1，TX PB15 / RX PA8，仍走 Type-C | 115200 8N1 |
| VW553 AT link | UART2 TX PA6 / RX PA7，连接方式见接线文档 | 115200 8N1 |

VW553 镜像只固定表中的 VW553 侧引脚；H77 侧引脚由 H77 平台配置决定，本文不展开。

`ttyUSB` 编号会随插拔、USB Hub 或虚拟机重新枚举变化，不能只凭
`/dev/ttyUSB0`/`/dev/ttyUSB1` 判断设备身份。烧录前必须结合 by-path、USB
拓扑和实际板卡连接确认端口，且不得误操作 H779 串口。

## 修改固件时构建镜像

从 `platform/GD32/gd32_os` 执行：

```bash
./cmake_build.sh vw553_at_gateway clean
./cmake_build.sh vw553_at_gateway
```

对应：

```text
APP=vw553_at_gateway
application: platform/GD32/gd32_os/MSDK/vw553_at_gateway
```

产物：

```text
platform/GD32/gd32_os/scripts/images/image-all.bin
```

`image-all.bin` 包含 MBL 与应用，烧录起点固定为 `0x08000000`。实际大小、SHA256、
ELF/BIN 路径和配置记录在：

```text
platform/GD32/gd32_os/MSDK/vw553_at_gateway/docs/manifest.json
```

不要使用旧的 `cmake --build cmake_build --target MSDK`、旧工程目录或临时 SDK 产物。

## 进入 ROM ISP

### 当前 LCKFB VW553 默认方式

1. 断开手机 BLE 连接，停止其它串口监听程序。
2. 按住 VW553 **BOOT0**。
3. 短按并松开 **RST**。
4. 继续按住 BOOT0，直到烧录工具输出 `ISP handshake ok`。
5. 握手成功后可以松开 BOOT0，工具会继续 loader 上传、范围擦除、写入和 verify。

### H77 控制复位的历史/条件入口

早期 GinoPi 可通过 H77 CLI 的 `diag_vw553_reset isp` 控制 VW553 NRST。
只有在当前 H77 固件确实包含该命令、复位接线成立且 BOOT0 已拉高时
才能使用。该方式不是当前 LCKFB 独立烧录的必要条件，也不是唯一入口。

## 官方工具烧录

使用官方 `GD32MassProductionTool.exe`：

1. Chip 选择 `GD32VW553`。
2. 选择 VW553 板载 Type-C/CH340 对应串口。
3. Download baudrate 选择 `2000000`。
4. Erase 选择 `Page Erase`，不要选择 `Chip Erase`。
5. Firmware 选择 H77 仓库中的稳定镜像。
6. 按住 BOOT0、短按 RESET，进入 ROM ISP 后开始下载。
7. 下载完成后释放 BOOT0 并再次复位。

## Linux/自动化备用烧录

如果无法运行官方 Windows GUI/CMD，可使用 GD32 平台仓库中的 Python ISP。
该方式需要 `platform/GD32` 仓库。确认串口没有被占用后，从 TuyaOpen 根目录执行：

```bash
python3 platform/GD32/platform_flash_bridge.py \
  --device GD32VW553H \
  --port /dev/ttyUSB0 \
  --baud 2000000 \
  --binfile platform/GD32H77X/wireless_fw/vw553/uart/vw553_at_gateway_uart_v0.5.2.bin
```

只有修改 VW553 源码并重新构建时，才把 `--binfile` 改为：

```text
platform/GD32/gd32_os/scripts/images/image-all.bin
```

把 `/dev/ttyUSB0` 替换为实际确认的 VW553 Type-C 端口。

Linux 后端固定调用：

```text
platform/GD32/tools/gd32_isp_flash.py
```

其擦除接口使用：

```text
loader.erase(flash_addr, len(image), chip_erase=False)
```

即只对镜像覆盖范围执行 4 KiB 扇区擦除，不执行 chip erase 或 mass erase。
不要改用全片擦除工具，也不要扩展到镜像范围之外，避免破坏设备校准或其它持久数据。

## 复位和启动验证

1. 烧录并 verify 成功后松开 BOOT0。
2. 短按一次 VW553 RST。
3. 从 VW553 Type-C 对应端口以 `115200 8N1` 只读采集日志。
4. 正常情况下可看到 RF、Wi-Fi calibration、PHY、BLE service 和 BLE adapter 初始化日志。

示例关键行：

```text
=== RF initialization finished ===
SDK Version: v1.0.3g-...
=== WiFi calibration done ===
=== PHY initialization finished ===
[ble_datatrans_srv_cb], svc_add_rsp status = 0x0
BLE local addr: ...
=== BLE Adapter enable complete ===
```

UART1 日志正常不等于 H779 AT 链路已经验证。AT 使用 UART2 PA6/PA7，115200；
必须另外连接 H779 并完成 AT 探测/业务验收。若日志为空，检查 BOOT0 是否已释放、
RST 是否确实发生、串口映射和波特率是否正确。

## 故障处理

- `No reply from the ROM bootloader`：确认 BOOT0 在 RST 释放后仍保持高电平，且选中的是 VW553 Type-C 端口。
- 烧录后无日志：释放 BOOT0、复位 VW553，再以 115200 8N1 采集。
- 有启动日志但 H779 无 AT：检查 UART2 PA6/PA7 交叉连接、共地和 115200 参数；日志口不能作为 AT 口。
- AT 超时或协议异常：先确认设备已退出 ISP 且 BOOT0 为低，再排查 AT 线和 H779 host。
- H779 DFU 拨码与 VW553 ISP 无关，不要把 H779 切入下载模式来尝试修复 VW553。
