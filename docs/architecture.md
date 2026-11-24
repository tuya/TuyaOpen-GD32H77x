# GD32H77X 平台架构

本文描述 GD32H77X TuyaOpen 平台的长期架构、目录边界、配置流程以及
GD32H779 与 GD32VW553 的职责划分。阶段性验证记录、交接材料和临时补丁不进入
本文。

## 目标与边界

- 主控使用 GD32H77X Cortex-M7，当前目标板为 `GD32H779V_START`，芯片为
  `GD32H779VW`。
- 无线协处理器使用 GD32VW553，H779 通过 UART AT 接口使用其 Wi-Fi、BLE、
  TCP/UDP 和配网能力。
- 平台基于 GigaDevice 指定原厂 SDK、FreeRTOS 11.2.0、GNU Arm GCC 和
  CMake/Ninja，不引入 RT-Thread SDK、设备框架或 SCons。
- 同一平台同时服务 TuyaOpen 应用和平台原生工程。TuyaOpen 应用不是特殊后端，
  只通过通用 project inputs 提供源码、头文件、预编译库和链接参数。
- 首个功能范围包含 BLE 配网、Wi-Fi AP 配网、STA 联网、云端激活、MQTT 和
  TLS/HTTP；两条配网路径均为正式验收项。

## 系统分层

```text
TuyaOpen application
    -> TAL / TKL
    -> tuyaos/tuyaos_adapter
    -> platform components / drivers / runtime
    -> FreeRTOS + GigaDevice SDK

H779 AT transport <-> GD32VW553 AT gateway <-> Wi-Fi / BLE / TCP / UDP
```

| 层 | 职责 |
| --- | --- |
| TuyaOpen 应用 | 业务、DP、配网状态机、Tuya TLS/MQTT 和云端协议 |
| `tuyaos/tuyaos_adapter` | TKL/TAL 到平台能力的唯一映射层，负责错误码和接口语义转换 |
| `components/at` | 平台中立的 AT client、parser、事务、事件队列和 socket 语义，不依赖 Tuya/TKL |
| `components/drivers/runtime` | GD32 外设、Flash/ECNVM、时钟、启动和板级运行时 |
| `project/*` | 独立工程入口；`tuya`、`at_bridge`、`at_reset` 和 `board_diag` 地位相同 |
| `gd32/` | GigaDevice 原厂 CMSIS、外设库和构建输入 |
| `components/FreeRTOS` | FreeRTOS kernel 和 Cortex-M7 port |

平台公共构建后端只消费每个工程 `project.cmake` 声明的通用输入，不硬编码
`tuya`、`tuya_app_main`、`libtuyaapp.a` 或 Tuya 专用无线组件。

## 目录结构

```text
platform/GD32H77X/
├── Kconfig
├── build_example.py
├── build_firmware.py
├── platform_config.py
├── docs/
├── gd32h77x_os/
│   ├── chips/<canonical-chip>/
│   ├── components/
│   │   ├── at/
│   │   ├── console/
│   │   ├── drivers/
│   │   ├── FreeRTOS/
│   │   ├── runtime/
│   │   └── startup/
│   ├── gd32/
│   ├── project/<project>/
│   └── tools/
└── tuyaos/tuyaos_adapter/
```

板级输入位于 TuyaOpen 主仓的 `boards/GD32H77X/<board>/`。板级可选配置只能通过
TuyaOpen 前端 staging 成构建目录下的 project config；`gd32h77x_os` 不发现板卡，
也不针对 TuyaOpen 做特殊判断。

## 配置流程

平台和 TuyaOpen 各自维护独立 Kconfig、配置生成器和头文件。平台配置按以下顺序
解析：

```text
platform/GD32H77X/Kconfig
-> gd32h77x_os/chips/<canonical-chip>/<canonical-chip>.config
-> project/<project>/project.config（可选）
-> generated/platform_config/gd32h77x_platform.config
-> generated/platform_config/gd32h77x_config.h
-> generated/platform_config/gd32h77x_config.cmake
```

TuyaOpen 构建时，如果板级存在可选覆盖，前端先执行：

```text
boards/GD32H77X/<board>/platform_config/gd32h77x.config
-> <build>/gd32h77x/project/tuya/project.config
```

平台随后把该文件作为普通 project override 加载。`tuya_kconfig.h` 只描述
TuyaOpen 通用配置和板型身份，不用于生成平台配置。

引脚、UART、DMA、Flash backend 和器件几何应由 Kconfig、芯片默认配置和板级
project config 管理；烧录设备自动枚举，SWD Pack、UART 波特率和 readback verify
允许通过主机命令临时覆盖。

## 构建与交付

- TuyaOpen 入口：`tos.py build` 调用 `build_example.py`，选择
  `gd32h77x_os/project/tuya`，把 TuyaOpen 头文件和静态库转成通用工程输入。
- 原生工程入口：`gd32h77x_os/tools/build_project.py`，不经过 `tos.py build`。
- 两个入口最终使用同一套 `build_firmware.py`、内存布局 resolver、链接模板和
  镜像打包/校验流程。
- QIO 是完整交付包，包含内部 boot/primary 镜像、ECNVM payload、ELF、layout
  report 和 manifest。烧录前必须重新校验布局、摘要和目标器件。
- 烧录默认只写允许的启动/应用/代码分区，不全片擦除，不覆盖授权、KV、OTA
  metadata、backup 或 scratch 分区。
- 不改 VW553 固件时，使用 `boards/GD32H77X/GD32H779V_START/firmware/vw553/` 中按 UART/SPI 分类的版本化
  预编译镜像；该目录不依赖 `platform/GD32` 源码构建。

## VW553 接口边界

| 功能 | VW553 侧 | 约束 |
| --- | --- | --- |
| AT UART | UART2 TX PA6 / RX PA7 | 115200 8N1；H77 侧引脚由其平台配置决定 |
| 调试日志 | UART1 TX PB15 / RX PA8 | 115200 8N1，连接板载 Type-C/CH340 |
| 模组复位 | NRST/RESET | 常规软复位优先通过 AT；硬件复位用于失联恢复 |
| SPI | 预留 | 当前不是产品数据通路，未完成组合验证 |

VW553 负责 Wi-Fi/BLE、STA/SoftAP、DHCP、TCP/UDP 和 AT gateway；H779 负责
TuyaOpen 应用、TLS/MQTT、AT 事务状态机和 socket 语义映射。BLE 配网与 AP 配网
必须使用同一套正式 AT 能力，不能靠扩大超时或盲目重试掩盖协议错误。

VW553 固件只定义 VW553 侧的 UART2 AT 引脚、日志 UART 和自身功能配置；H77 侧
引脚由 GD32H77X 平台配置决定。两端必须匹配，但不存在由 VW553 镜像配置 H77
引脚的关系。H77 侧接线只在独立接线文档中记录。

AT 层必须保持以下边界：

- 串行 AT 命令与异步 `+IPD`/URC 事件分离，不能互相污染事务结果。
- send/recv/accept 的字节计数和错误码使用不同返回语义；长度接口使用 `int`，
  不允许使用窄枚举或小整数承载 payload 长度。
- transport poison 后 fail closed，明确恢复；恢复过程不得静默丢弃应用数据。
- socket 映射、事件队列和 AT 状态机位于平台中立层，Tuya/TKL 映射只放在
  `tuyaos/tuyaos_adapter`。

## Flash 与内存

芯片默认分区、板级覆盖、CNVM/ECNVM 路由、KV 和烧录约束见
[Flash 分区与板级覆盖](flash-layout.md)。关键原则：

- 分区由 `partitions.csv` 和 `flash_devices.csv` 显式描述，不做字段级隐式合并。
- linker、启动代码和 CPU 映射只使用可执行物理地址；存储 logical address 不能
  直接转换为指针。
- ECNVM 擦写命令在 AXI SRAM 执行并关闭中断；擦写期间不能从目标区域取指或读取。
- 存在 OTA 分区不代表 OTA 下载、安装、回滚和掉电恢复已经实现或验收。

## 许可边界

- Tuya 平台自有代码使用 `platform/GD32H77X/LICENSE` 中的 Apache-2.0。
- `gd32/` 下 GigaDevice 提供的 SDK、CMSIS、外设库和文档受
  `gd32/SOFTWARE LICENSE AGREEMENT SLA-GD0001-version1.1.pdf` 约束。
- FreeRTOS 和其他第三方组件分别遵循各自目录中的 License，上述平台 License
  不覆盖它们。

## 相关文档

- [平台说明](../README.md)
- [Flash 分区与板级覆盖](flash-layout.md)
- [H779V-START 与 VW553 接线](gd32h779v_start_vw553_wiring.md)
- [VW553 wireless firmware](../../../boards/GD32H77X/GD32H779V_START/firmware/vw553/README.md)
- [VW553 首期无线适配清单](vw553-capabilities.md)
- [VW553 UART ISP 烧录](vw553-isp-flashing.md)
