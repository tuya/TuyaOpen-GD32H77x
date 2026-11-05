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

## SPI 与握手信号预留

以下 H779 信号已经按原理图核对，作为后续 SPI 桥接的预留焊线。现阶段尚未验证 GD32H779V-START 与嘉立创 GD32VW553 的 SPI 通信；预留接线不表示桥接驱动或 553 AT 固件已经适配。

| 功能 | GD32H779V-START | GD32VW553 侧信号 | 状态与说明 |
| --- | --- | --- | --- |
| SPI 时钟 | SPI3_SCK，PE2，AF5；JP5-16 | SPI_SCK | 预留。 |
| 主控输入、从片输出 | SPI3_MISO，PE5，AF5；JP4-29 | SPI_MISO | 预留。 |
| 主控输出、从片输入 | SPI3_MOSI，PE6，AF5；JP5-24 | SPI_MOSI | 预留。 |
| 片选或协议控制 | PE4 GPIO；JP5-22 | SPI_CS 或协议定义的控制信号 | 预留。不要据此假定是标准 NSS 或固定有效电平。 |
| READY / 中断 | PE3 GPIO、EXTI3；JP4-19 | READY / 中断输出 | 预留；极性和时序须按双方固件协议确认。 |

旧参考工程的 553 SPI 信号为 PA2=SCK、PA1=MISO、PA0=MOSI、PA3=master_write、PA4=ready，但不能直接照此焊到嘉立创板：该板 PA0 接按键、PA1 接 LED。若要采用这组信号，需先处理板上负载并确认适用的 553 固件及引脚配置。当前只保留 H779 侧这五根线的焊接位置，不据旧工程推定 553 侧可直接连接。

## 串口、调试与下载保留

H779 板的 PC6/PC7（USART5）连接 CN4 板载 USB-UART，用作 115200、8N1 日志口，保留不占用。PA13/PA14 SWD 保留给 GDLink。

嘉立创 GD32VW553 板的 PB15/PA8 已连接板载 CH340。应用态由 UART1 作为 115200 日志口；进入 ROM ISP 后由 BootROM 的 USART0 路径复用同一组物理引脚。H779 AT 链路使用独立的 UART2 PA6/PA7，不与 Type-C 日志口共用信号。

当前 AT 固件已经固定为 UART2：553 PA6_TX 接 H779 PB12_RX，553 PA7_RX 接 H779 PB13_TX。不要把 UART2 AT 线改接到 Type-C 日志脚。

553 板 BOOT0 保持为板上独立的烧录恢复入口；高电平配合 RST 进入 ROM ISP，正常运行时保持低电平。不要将它接到 H779 BOOT 或焊死为某一电平。

## 软件绑定与验证边界

当前仓库中的 H779 板级取值位于 [GD32H779VW.config](../gd32h77x_os/chips/GD32H779VW/GD32H779VW.config)，使用 UART4 PB13/PB12、AF14 和 PC5 复位；可选值和取值范围定义在 [platform Kconfig](../Kconfig)。该绑定只说明 H779 侧 UART 配置，不代表 553 侧 AT 固件引脚已确认或两板已连通。

UART 主链路已经按 UART4 ↔ VW553 UART2 完成联调。SPI/握手信号仅完成 H779 侧预留核对，SPI 桥接和对应 VW553 固件尚未实现或验收，不能将旧参考工程的通信结果视为本板验证结果。稳定 UART 固件见 [VW553 wireless firmware](../wireless_fw/vw553/README.md)。

## 参考资料

- H779：GigaDevice `GD32H779V-START V1.0` 原理图，第 7 页 Extension；板载 USB-UART 见 CN4。
- 553：仓库中的 [嘉立创 GD32VW553 资料](../../../boards/GD32/LCKFB_GD32VW553/LCKFB_GD32VW553.pdf)。这是彩色引脚与资源介绍资料，不是电路原理图，不能据此编造排针编号。
- 553 串口说明：[立创 GD32VW553 UART 通信](https://wiki.lckfb.com/zh-hans/gd32vw553/beginner/uart-communication.html)。
- 553 PA0/PA1 板载功能：[立创 GD32VW553 按键与 LED](https://wiki.lckfb.com/zh-hans/gd32vw553/beginner/key-led.html)。
