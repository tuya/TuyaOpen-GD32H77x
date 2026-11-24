# AT 通道与引脚路由

AT 驱动不区分 START、Desk Buddy 等板型。平台 Kconfig 选择 UART/SPI、
外设实例和各信号引脚，`platform_config.py` 在生成头文件前校验路由。
START 的公共 Wi-Fi 接线保存在
`boards/GD32H77X/GD32H779V_START/platform_config/gd32h77x.config`，
各应用直接继承板级配置，驱动中没有板型预设分支。

## SPI 示例

当前 START 验证接线对应以下配置；修改外设时必须同时提供匹配的信号路由：

```ini
CONFIG_ENABLE_GD32H77X_AT_TRANSPORT_SPI=y
CONFIG_ENABLE_GD32H77X_AT_SPI_CS_ONLY=y
CONFIG_GD32H77X_AT_SPI_NUMBER=1
CONFIG_GD32H77X_AT_SPI_SCK="PB13"
CONFIG_GD32H77X_AT_SPI_MISO="PB14"
CONFIG_GD32H77X_AT_SPI_MOSI="PB15"
CONFIG_GD32H77X_AT_SPI_CS="PC0"
CONFIG_ENABLE_GD32H77X_AT_RESET_GPIO_PORT_C=y
CONFIG_GD32H77X_AT_RESET_PIN_NUMBER=5
```

`ENABLE_GD32H77X_AT_SPI_PSC_64/128/256` 选择分频。
CS 为普通低有效 GPIO，不要求硬件 NSS 引脚。
旧协议另配 `GD32H77X_AT_SPI_READY`；CS_ONLY 协议不初始化或占用 READY。
VW553 固件必须使用相同协议版本。

## UART 示例

```ini
CONFIG_ENABLE_GD32H77X_AT_TRANSPORT_UART=y
CONFIG_ENABLE_GD32H77X_AT_UART_USART0=y
CONFIG_GD32H77X_AT_UART_TX_ROUTE="PA9"
CONFIG_GD32H77X_AT_UART_RX_ROUTE="PB7"
CONFIG_GD32H77X_AT_BAUD=115200
```

TX/RX 可以跨端口、使用不同 AF；用户不需要填写 AF。
为兼容已有配置，某个 ROUTE 为空时，该信号仍使用旧的 AT_GPIO_PORT、
TX/RX_PIN_NUMBER 和 AF_NUMBER，但同样会检查是否合法。

## 自动派生与校验范围

- UART：各引脚的 GPIO 时钟、AF，串口时钟、IRQ、RX DMA 请求。
- SPI：各引脚的 GPIO 时钟、AF，SPI 时钟源、门控、复位及 TX/RX DMA 请求。
- 检查活动 AT 通道、复位、控制台之间的引脚重复和 UART 实例冲突。
- 检查 H779 V/Z/I 封装：例如 V/Z 的 PC2/PC3 普通焊盘未引出。
- 非法信号路由在配置生成阶段报错，错误消息列出该信号已收录的可用引脚。

路由目录在 `at_routes.py`，依据
`boards/GD32H77X/doc/DS-GD32H779xx-Datasheet-EN-Rev0.5.pdf`
的引脚定义及 AF 表收录已核对的普通焊盘路由。目录不是完整芯片 pinmux 表；
未收录路由会拒绝，需要核对资料后补充。其他芯片型号暂不执行 H779 封装检查。
PC2_C/PC3_C 涉及特殊焊盘连接，尚未加入；新版 Desk Buddy 的
PC2/PC3 普通焊盘按 I 封装支持，不按 `_C` 焊盘处理。

当前没有全工程引脚所有权表，LCD、SDIO 等其他外设与 AT 的占用冲突仍需
结合板级配置检查。旧的 `AT_SPI_START_DESK_SIM` 已移除，使用它的项目
应迁移到上面的显式 SPI1 路由。

这次配置重构不改变 SPI 帧协议、DMA 缓冲处理和中断收发流程。
