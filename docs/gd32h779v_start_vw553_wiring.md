# GD32H779V-START 与 GD32VW553 接线记录

本文记录 GD32H779V-START（MCU：GD32H779VWT7）与嘉立创 GD32VW553 开发板的接线方案，供焊线和后续联调查阅。H779 侧引脚依据原厂 GD32H779V-START 原理图第 7 页 Extension 核对，并由 H779 平台配置决定；VW553 侧引脚依据当前 `vw553_at_gateway` 固件配置核对。两侧引脚是独立配置，接线必须匹配，但任何一侧固件都不能远程决定另一侧的引脚选择。

## UART 与复位接线方案

两块开发板分别由各自 USB 供电，信号地相连。IO 为 3.3 V TTL。H779 板 JP4-6 是板侧 3.3 V 引出位置，本方案不将它接到 553 板电源：不要并接两块板的 3.3 V 稳压器输出。

| 功能 | GD32H779V-START | GD32VW553 板侧 | 状态与说明 |
| --- | --- | --- | --- |
| AT UART：H779 TX | UART4_TX，PB13，AF14；JP5-27 | UART2_RX，PA7 | 115200 8N1；当前 UART gateway 已验证。 |
| AT UART：H779 RX | UART4_RX，PB12，AF14；JP5-25 | UART2_TX，PA6 | 115200 8N1；当前 UART gateway 已验证。 |
| 从片复位 | PC5 GPIO；JP4-20 | NRST / RESET | 已接入；常规使用 AT 软复位，失联时由 H779 拉低复位。 |
| 共地 | GND；JP4-5 | GND | 必须连接。 |
| H779 板侧 3.3 V 位置 | 3V3；JP4-6 | — | 仅作位置记录；不要与另一块板的 3.3 V 输出相连。 |

H779 排针位置按板上 1 脚标记和原理图编号确认，不要根据照片左右方向推断脚号。

### VW553 侧引脚

| 用途 | 外设/模式 | TX | RX | 参数 | 说明 |
| --- | --- | --- | --- | --- | --- |
| H779 AT 链路 | UART2 | `PA6`，AF10 | `PA7`，AF8 | 115200 8N1 | 只承载 AT 命令、响应和二进制帧，禁止输出日志文本 |
| 调试日志 | UART1 | `PB15`，AF7 | `PA8`，AF3 | 115200 8N1 | 连接板载 Type-C/CH340 |
| ROM ISP | BootROM USART0 | `PB15` | `PA8` | 初始 57600 8E1 | 与应用态日志复用同一组物理引脚；loader 阶段为 115200 8N1 |
| 复位 | NRST/RESET | — | — | 低有效 | 可由 H779 `PC5` 驱动，也可使用板上 RST |
| 启动模式 | BOOT0 | — | — | 高电平进入 ISP | 正常运行时保持低电平，不与 H779 启动脚连接 |

## SPI 与握手信号

H779 SPI transport 和 VW553 SPI gateway 已加入源码，均为显式 opt-in；UART 仍是默认链路。下表是 SPI transport 使用的五根信号线。双板已通过基础 AT 与 `AT+GMR` smoke；逻辑分析仪波形和电气时序裕量尚未验证。

| 功能 | GD32H779V-START | GD32VW553 侧信号 | 状态与说明 |
| --- | --- | --- | --- |
| SPI 时钟 | SPI3_SCK，PE2，AF5；JP5-16 | PA2 | H779 master，Mode 0，MSB first，8-bit。 |
| 主控输入、从片输出 | SPI3_MISO，PE5，AF5；JP4-29 | PA1 | VW553 MISO。 |
| 主控输出、从片输入 | SPI3_MOSI，PE6，AF5；JP5-24 | PA0 | VW553 MOSI。 |
| REQUEST / 软件片选 | PE4 GPIO；JP5-22 | PA3 输入 | H779 每笔事务前拉高，完成后拉低；不是 SPI NSS。 |
| READY | PE3 GPIO、EXTI3；JP4-19 | PA4 输出 | VW553 收到 REQUEST 后先准备 TX/RX，再拉高 READY；DMA 收发完成且 REQUEST 撤销后拉低。CRC/ACK 状态在下一帧体现；空 poll 也必须响应。 |
| 共地 | GND；JP4-5 | GND | SPI 必须共地；IO 为 3.3 V。 |

仓库 LCKFB_GD32VW553.pdf 的引脚图标注 PA0 为用户按键、板载 LED 为 PC13；PA1 用于 SPI MISO。使用 SPI 时不要按 PA0 的 KEY。553 侧按本协议将 PA3 用作 REQUEST 输入、PA4 用作 READY 输出，不能把旧 master/slave 示例的 GPIO 方向直接照搬。

### SPI 帧与握手

H779 每次拉高 PE4 后等待 PE3 READY 高，再以全双工方式传输固定 256 字节，随后拉低 PE4 并等待 READY 低。SPI3 当前通过 APB2 时钟源运行，Kconfig 默认分频 `/256`，也可选 `/128` 或 `/64`；实际 SCK 频率为所选 APB2 SPI 时钟除以该分频值。

每帧由 16 字节头和最多 240 字节 payload 组成；未使用 payload 区补零。头字段均为小端：`A7 5C` magic、版本 `1`、flags、TX sequence、累计 ACK（下一期待序号）、payload 长度、两个保留零字节、payload CRC16、header CRC16。CRC 使用 CRC-16/CCITT-FALSE（初值 `FFFF`、多项式 `1021`），header CRC 覆盖前 14 字节，payload CRC 仅覆盖有效 payload。flags `0x01` 为 HELLO，`0x02` 为 HELLO_ACK，`0` 为数据或空 poll。

两方向各自停等发送：同一方向只保留一帧未确认 payload，直到对端 ACK 前保持序号和数据不变重传；接收方只投递下一期待序号，重复帧不重复投递。启动通过 HELLO/HELLO_ACK 同步；运行中收到已建链对端的 HELLO 时清空旧 in-flight 数据并重新握手。H779 可用空 poll 读取异步 AT 响应和 URC。

## 串口、调试与下载保留

H779 板的 PC6/PC7（USART5）连接 CN4 板载 USB-UART，用作 115200、8N1 日志口，保留不占用。PA13/PA14 SWD 保留给 GDLink。

嘉立创 GD32VW553 板的 PB15/PA8 已连接板载 CH340。应用态由 UART1 作为 115200 日志口；进入 ROM ISP 后由 BootROM 的 USART0 路径复用同一组物理引脚。UART 模式下 H779 AT 链路使用独立的 UART2 PA6/PA7，不与 Type-C 日志口共用信号。

UART 模式下 553 AT 固件使用 UART2：PA6_TX 接 H779 PB12_RX，PA7_RX 接 H779 PB13_TX。不要把 UART2 AT 线改接到 Type-C 日志脚。

553 板 BOOT0 保持为板上独立的烧录恢复入口；高电平配合 RST 进入 ROM ISP，正常运行时保持低电平。不要将它接到 H779 BOOT 或焊死为某一电平。

## 软件绑定与验证边界

当前仓库中的 H779 板级默认值位于 [GD32H779VW.config](../gd32h77x_os/chips/GD32H779VW/GD32H779VW.config)，使用 UART4 PB13/PB12、AF14 和 PC5 复位；可选值和取值范围定义在 [platform Kconfig](../Kconfig)。SPI 仍需在独立 project config 中选择 `ENABLE_GD32H77X_AT_TRANSPORT_SPI`；默认 UART 配置不变。探针工程位于 `gd32h77x_os/project/spi_at_probe`，使用 USART5 PC6/PC7 控制台，发送 `AT` 和 `AT+GMR`。从仓库根目录构建：

```bash
python3 platform/GD32H77X/gd32h77x_os/tools/build_project.py \
  platform/GD32H77X/gd32h77x_os/project/spi_at_probe \
  --chip GD32H779VW --board GD32H779V_START
```

VW553 SPI 候选固件及 SHA256 见 [SPI gateway 候选固件](../wireless_fw/vw553/spi/README.md)，其源码构建入口见 [VW553 AT gateway](../../GD32/gd32_os/MSDK/vw553_at_gateway/docs/README.md)。SPI 候选保持普通 AT/TCP/IP/BLE 字节流，但不支持依赖 UART idle 检测的 `AT+CIPMODE=1` 与 BLE passthrough。

SPI 基础实机 smoke 记录：H779 通过 `AT` 收到 `OK`，`AT+GMR` 返回
`v1.0.3g-04f35186ae4a8741`；探针结果 `SPI_AT_PROBE_RESULT PASS at=1 version=1`，
诊断统计 `timeouts=0`、`invalid=0`、`valid=69`。首次冷启动尝试失败且原因未定；
在 VW553 正常启动后复位 H779，探针通过，尚不能声称冷启动稳定。二进制 payload、
Socket/BLE 业务、吞吐和长稳均未验收，逻辑分析仪波形与电气裕量也未确认。UART
主链路已按 UART4 ↔ VW553 UART2 完成联调；稳定 UART 固件见
[VW553 wireless firmware](../wireless_fw/vw553/README.md)。

### 启动与复位恢复补充（2026-11-12）

H779 transport 在启动握手阶段会继续发送 HELLO，READY 超时不会提前结束已排队的写操作，最终仍遵守调用者的 write deadline。每次传输前先保持 REQUEST 低并等待 READY 低（最多 1200 ms），避免把上次未完成 DMA 的 READY 高电平当成新事务。握手期无效帧继续计数并尝试有界重连；已建链后仍按 transport fault 结束不确定写操作。周期探针源码见 [`spi_at_probe.c`](../gd32h77x_os/project/spi_at_probe/spi_at_probe.c)。

软件复位恢复实测：H779 独立复位时 VW553 保持运行，H779 随后的 AT/GMR 周期成功；在 VW553 通过 PC5/NRST 复位期间，H779 两个新 AT 周期分别在约 15 秒 write deadline 后如实失败，VW553 启动后新的 AT/GMR 周期恢复成功，后续周期持续通过。双方顺序复位（先将 VW553 NRST 拉低，再复位 H779；H779 GPIO 复位释放 NRST）时，两端启动日志均捕获，H779 首轮 AT/GMR 在 524 ms 通过。逐周期串口日志、测试摘要和镜像哈希保存在仓库忽略目录 `.cache/gino-validation/spi-at-startup-20261112/`。

上述结果只覆盖软件 reset、SPI 握手和只读 AT/GMR。实际 USB 断电冷启动未验证：本轮 10 分钟热插拔捕获窗口内未观察到 USB 断开/重枚举或用户断电动作，需在用户准备好后重新启动捕获再测；socket、吞吐、长稳与电气波形仍未验收。

## 参考资料

- H779：GigaDevice `GD32H779V-START V1.0` 原理图，第 7 页 Extension；板载 USB-UART 见 CN4。
- 553：仓库中的 [嘉立创 GD32VW553 资料](../../../boards/GD32/LCKFB_GD32VW553/LCKFB_GD32VW553.pdf)。这是彩色引脚与资源介绍资料，不是电路原理图，不能据此编造排针编号。
- 553 串口说明：[立创 GD32VW553 UART 通信](https://wiki.lckfb.com/zh-hans/gd32vw553/beginner/uart-communication.html)。
- 553 按键与 LED 板载功能：[立创 GD32VW553 按键与 LED](https://wiki.lckfb.com/zh-hans/gd32vw553/beginner/key-led.html)。
