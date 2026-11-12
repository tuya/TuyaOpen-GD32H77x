# GD32H77X TKL pinmux

## 编号与职责

`TUYA_PIN_NAME_E` 使用 `TUYA_GPIO_NUM_E` 的连续编号：

| 端口 | Tuya GPIO ID |
|---|---|
| PA0-PA15 | 0-15 |
| PB0-PB15 | 16-31 |
| PC0-PC15 | 32-47 |
| PD0-PD15 | 48-63 |
| PE0-PE15 | 64-79 |
| PF0-PF15 | 80-95 |
| PG0-PG15 | 96-111 |
| PH0-PH15 | 112-127 |
| PJ0-PJ15 | 128-143 |
| PK0-PK15 | 144-159 |

引脚默认值属于 TKL 驱动。`tkl_pinmux` 只负责在外设初始化前修改驱动里的默认表；
`tkl_i2c_init()`、`tkl_uart_init()`、`tkl_dvp_init()`、`tkl_adc_init()` 不再调用公共
`tkl_io_pinmux_config()` 或 `tkl_multi_io_pinmux_config()`，而是直接应用最终表。
公共枚举保留 `TUYA_DVP0_MCLK`，用于后续 MCLK 可路由的平台。GD32H77X 的 DVP MCLK
是 CKOUT0 固定输出，不提供 pinmux 路由。`tkl_dvp_init()` 根据
`TUYA_DVP_CFG_T.inter_cfg.sensor_clk` 在 TKL 内部配置时钟和固定引脚，板级不再注入
MCLK 回调。

## I2C 默认引脚

| 控制器 | SCL | SDA | AF |
|---|---|---|---|
| I2C0 | PB8 / TUYA_GPIO_NUM_24 | PB9 / TUYA_GPIO_NUM_25 | 4 |
| I2C1 | PF1 / TUYA_GPIO_NUM_81 | PF0 / TUYA_GPIO_NUM_80 | 4 |
| I2C2 | PH7 / TUYA_GPIO_NUM_119 | PH8 / TUYA_GPIO_NUM_120 | 4 |
| I2C3 | PD12 / TUYA_GPIO_NUM_60 | PD13 / TUYA_GPIO_NUM_61 | 4 |

I2C 引脚配置为复用开漏并开启内部上拉。外部上拉仍由板级硬件提供。

## UART 默认引脚

TKL UART 编号与芯片外设一一对应。平台 console binding 独立配置，不会改变
`TUYA_UART_NUM_x` 对应的物理外设。

| TKL UART | 外设 | TX | RX | AF |
|---|---|---|---|---|
| UART0 | USART0 | PA9 / TUYA_GPIO_NUM_9 | PA10 / TUYA_GPIO_NUM_10 | 7 |
| UART1 | USART1 | PA2 / TUYA_GPIO_NUM_2 | PA3 / TUYA_GPIO_NUM_3 | 7 |
| UART2 | USART2 | PH2 / TUYA_GPIO_NUM_114 | PH4 / TUYA_GPIO_NUM_116 | 1 |
| UART3 | UART3 | PA0 / TUYA_GPIO_NUM_0 | PA1 / TUYA_GPIO_NUM_1 | 8 |
| UART4 | UART4 | PB13 / TUYA_GPIO_NUM_29 | PB12 / TUYA_GPIO_NUM_28 | 14 |
| UART5 | USART5 | PC6 / TUYA_GPIO_NUM_38 | PC7 / TUYA_GPIO_NUM_39 | 7 |
| UART6 | UART6 | PE8 / TUYA_GPIO_NUM_72 | PE7 / TUYA_GPIO_NUM_71 | 7 |
| UART7 | UART7 | PE1 / TUYA_GPIO_NUM_65 | PE0 / TUYA_GPIO_NUM_64 | 8 |

各控制器还提供常见的备用引脚路由，例如 USART0 的 PB6/PB7、PF4/PF5，
USART1 的 PD5/PD6，USART2 的 PB10/PB11、PD8/PD9，UART3 的 PC10/PC11、
PD0/PD1。`tkl_uart_init()` 按最终表配置 GPIO 和 UART 参数。

在启用平台 AT transport 的配置里，UART4 由 AT 强定义的 IRQ handler 接管；
TKL 的 UART4 handler 为弱定义，避免同一物理外设的链接冲突。

## PWM 默认引脚

TKL PWM0..11 映射到 TIMER0/1/2 的各 4 个通道。同一定时器上的通道共享频率和计数模式；
驱动分别维护通道启停状态，停止一个通道不会误停同组其他通道。

| TKL PWM | 定时器通道 | 引脚 | Tuya GPIO ID | AF |
|---|---|---|---|---|
| PWM0 | TIMER0_CH0 | PA8 | 8 | 1 |
| PWM1 | TIMER0_CH1 | PA9 | 9 | 1 |
| PWM2 | TIMER0_CH2 | PA10 | 10 | 1 |
| PWM3 | TIMER0_CH3 | PA11 | 11 | 1 |
| PWM4 | TIMER1_CH0 | PA0 | 0 | 1 |
| PWM5 | TIMER1_CH1 | PA1 | 1 | 1 |
| PWM6 | TIMER1_CH2 | PA2 | 2 | 1 |
| PWM7 | TIMER1_CH3 | PA3 | 3 | 1 |
| PWM8 | TIMER2_CH0 | PA6 | 6 | 2 |
| PWM9 | TIMER2_CH1 | PA7 | 7 | 2 |
| PWM10 | TIMER2_CH2 | PB0 | 16 | 2 |
| PWM11 | TIMER2_CH3 | PB1 | 17 | 2 |

## DVP/DCI 默认引脚

| 信号 | 引脚 | Tuya GPIO ID | AF |
|---|---|---|---|
| D0 | PC6 | 38 | 13 |
| D1 | PC7 | 39 | 13 |
| D2 | PB13 | 29 | 13 |
| D3 | PH12 | 124 | 13 |
| D4 | PH14 | 126 | 13 |
| D5 | PD3 | 51 | 13 |
| D6 | PB8 | 24 | 13 |
| D7 | PB9 | 25 | 13 |
| PIXCLK | PA6 | 6 | 13 |
| HSYNC | PH15 | 127 | 1 |
| VSYNC | PB7 | 23 | 13 |
| MCLK | PA8 | 8 | 0 (CKOUT0，固定由 TKL 管理) |

## SDIO0/SDIO1 路由

平台 Kconfig 的 `SDIO card SDIO host` 选择 FatFS 后端使用的控制器：
`ENABLE_GD32H77X_SDIO_HOST_0` 或 `ENABLE_GD32H77X_SDIO_HOST_1`。

### SDIO0

| 信号 | 可选引脚 | AF |
|---|---|---|
| CLK | PC12 / TUYA_GPIO_NUM_44 | 12 |
| CMD | PD2 / TUYA_GPIO_NUM_50 | 12 |
| D0 | PB13 / TUYA_GPIO_NUM_29 | 12 |
| D0 | PC8 / TUYA_GPIO_NUM_40 | 12 |
| D1 | PC9 / TUYA_GPIO_NUM_41 | 12 |
| D2 | PD8 / TUYA_GPIO_NUM_56 | 6 |
| D2 | PC10 / TUYA_GPIO_NUM_42 | 12 |
| D3 | PD9 / TUYA_GPIO_NUM_57 | 6 |
| D3 | PC11 / TUYA_GPIO_NUM_43 | 12 |

Watch EVAL 默认使用 PC12/PD2/PC8/PC9/PC10/PC11。

### SDIO1

默认 TKL 引脚组为 PC1/PA0/PB14/PB15/PB13/PB4；pinmux 同时支持
PF4/PF5/PG9/PG10/PG11/PG12 备选路由。

## ADC 默认通道

ADC 初始化和读数使用同一张 `控制器 + 通道 + GPIO` 表。模拟模式不需要 AF。

### ADC0

| 通道 | 引脚 | Tuya GPIO ID | 通道 | 引脚 | Tuya GPIO ID |
|---|---|---|---|---|---|
| 2 | PF11 | 91 | 3 | PA6 | 6 |
| 4 | PC4 | 36 | 5 | PB1 | 17 |
| 6 | PF12 | 92 | 7 | PA7 | 7 |
| 8 | PC5 | 37 | 9 | PB0 | 16 |
| 10 | PC0 | 32 | 11 | PC1 | 33 |
| 14 | PA2 | 2 | 15 | PA3 | 3 |
| 16 | PA0 | 0 | 17 | PA1 | 1 |
| 18 | PA4 | 4 | 19 | PA5 | 5 |

### ADC1

| 通道 | 引脚 | Tuya GPIO ID | 通道 | 引脚 | Tuya GPIO ID |
|---|---|---|---|---|---|
| 2 | PF13 | 93 | 3 | PA6 | 6 |
| 4 | PC4 | 36 | 5 | PB1 | 17 |
| 6 | PF14 | 94 | 7 | PA7 | 7 |
| 8 | PC5 | 37 | 9 | PB0 | 16 |
| 10 | PC0 | 32 | 11 | PC1 | 33 |
| 13 | PC3 | 35 | 14 | PA2 | 2 |
| 15 | PA3 | 3 | 18 | PA4 | 4 |
| 19 | PA5 | 5 |  |  |  |

### ADC2

| 通道 | 引脚 | Tuya GPIO ID | 通道 | 引脚 | Tuya GPIO ID |
|---|---|---|---|---|---|
| 0 | PC2 | 34 | 1 | PC3 | 35 |
| 2 | PF9 | 89 | 3 | PF7 | 87 |
| 4 | PF5 | 85 | 5 | PF3 | 83 |
| 6 | PF10 | 90 | 7 | PF8 | 88 |
| 8 | PF6 | 86 | 9 | PF4 | 84 |
| 10 | PC0 | 32 | 11 | PC1 | 33 |
| 13 | PH2 | 114 | 14 | PH3 | 115 |
| 15 | PH4 | 116 | 16 | PH5 | 117 |

## 查询与批处理

`tkl_multi_io_pinmux_config` 在写寄存器前完成整批校验，并拒绝重复引脚。
`tkl_io_pin_to_func` 优先返回最近一次成功配置的功能；没有缓存时返回该引脚上
编码最小的已支持路由。查询不读回 MCU 复用寄存器。

主机回归覆盖 GPIO 连续编号、I2C/UART/DVP 复用模式、ADC 模拟模式，以及
`tkl_io_pin_to_func` 的查询结果：

```sh
platform/GD32H77X/tests/run_tkl_pinmux_host_test.sh
```

该测试验证驱动的路由表与模式选择，不替代真实板级电气验证。
