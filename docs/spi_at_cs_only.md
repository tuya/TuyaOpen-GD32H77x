# SPI AT v2：无 READY、低有效 CS 轮询

状态：两端实现、主机测试、CS-only v2 实机边界回环及已绑定应用自动联网验证通过；
新一轮配网、用户 DP 控制、异常事务实机注入及长稳尚待验证。
旧版 READY 协议的联网和 1248 项回环结果不能算作本协议的结果。

## 开发板接线

| 信号 | H779V_START | VW553 | 方向 |
| --- | --- | --- | --- |
| SCK | PE2 | PA2 | 主机 → 从机 |
| MOSI | PE6 | PA0 | 主机 → 从机 |
| MISO | PE5 | PA1 | 从机 → 主机 |
| CS（原 REQUEST） | PE4 | PA3/NSS | 主机 → 从机，低有效 |
| RESET | PC5 | NRST | 沿用既有板级复位配置 |
| GND | GND | GND | 共地 |

PE3 ↔ PA4 的 READY 线不再需要，也不会被新代码配置、驱动或读取。
这些是现有开发板接线，不是尚待确认的 Desk Buddy 下一版引脚适配。
CS 建议有外部上拉，避免主机复位/烧录时浮空选中从机。

## START 模拟下一版 Desk Buddy 的接线配置

该配置用于验证 SPI1 + CS-only + 553 PA4/PA5 软件链路，不验证 BGA176 的
PC3/PC3_C 同时使用、PH10/PU 电路或新板供电/射频。H779 选择
`CONFIG_ENABLE_GD32H77X_AT_SPI_START_DESK_SIM=y`（依赖 CS_ONLY）。
原有 SPI3/PE 引脚配置仍可通过关闭该选项恢复。

| 信号 | START 引脚 | 排针位置 | 553 引脚 |
| --- | --- | --- | --- |
| SPI1 SCK / AF5 | PB13 | JP5-27 | PA2 / AF5 |
| SPI1 MOSI / AF5 | PB15 | JP5-36 | PA4 / AF2 |
| SPI1 MISO / AF5 | PB14 | JP5-28 | PA5 / AF4 |
| CS GPIO | PC0 | JP4-22 | PA3 / AF5 NSS |
| RESET | PC5 | JP4-20 | 保留现有 NRST 线，不等同新板 PU 电路 |
| GND | GND | JP4-5 | GND |

SPI1 使用 PLL0Q（600 MHz 系统配置下为 300 MHz）及原 /256 分频，不将
SPI3 的 APB2 选择值直接套用到 SPI1。DMA 请求同步切到 SPI1_TX/RX。

553 额外设置 `VW553_GATEWAY_SPI_DESK_BUDDY_PINS=ON`，PA4/PA5 各自设置
AF2/AF4，不统一设置 AF5。该选项禁止与使用 PA4 READY 的旧模式同时开启。
原有 PA0/PA1 配置可通过关闭此选项恢复。

```sh
cd platform/GD32/gd32_os
VW553_GATEWAY_TRANSPORT=SPI VW553_GATEWAY_SPI_CS_ONLY=ON \
  VW553_GATEWAY_SPI_DESK_BUDDY_PINS=ON VW553_GATEWAY_DIAGNOSTICS=ON \
  ./cmake_build.sh vw553_at_gateway
```

独立回环项目为 `gd32h77x_os/project/spi_at_start_desk_probe`，复用原有
1248 项边界矩阵；该项目的结果必须与 SPI3/PA0/PA1 结果分开记录。

实机结果（2026-11-14）：已在上述 SPI1/PC0/PA4/PA5 接线上运行完整矩阵，
1248 项全部通过，0 失败，序号 1–1248 的采集记录完整，无重复/缺失。
CRC32、逐字节比较、结束标记及源/目标缓冲区哨兵均通过。
结束时只读采样为 DMA 完成 28787 帧、超时/错误 0、READY 计数 0；
有效帧 23328、拒收的无效快照 5458（各字段采样期间仍可能继续增长）。
不能将无效快照一律归因于从机忙或声称物理事务零失败；本轮应用数据经重传完整通过。
原始记录及矩阵检查结果位于 `.cache/gino-validation/start-desk-sim-20261114/`。

回环后已恢复并读回校验该配置的 switch_demo：自动连接 SST_Home，获取
192.168.50.32；约启动 10 秒时 MQTT TLS、MQTT_CONNECTED 和三个 SUBACK 成功，
云时间同步成功。原绑定 KV 保留，无需重新提供凭据。应用日志为同目录
`*-switch.log`，烧录日志为 `switch-flash.log`。这次未重新配网或验收用户 DP 控制。
既有 AT+RST/READY 首轮交错失败后异步恢复现象仍存在，不因此关闭复位待办。

此模拟只针对 Wi-Fi：PB13 在目标新板另用于摄像头 DCI_D2，不能据此验证
目标板摄像头与 Wi-Fi 的引脚共存；实际 Desk Buddy 仍需使用其原定 PC 引脚。

## CS-only 事务规则

- Mode 0，当前仍使用 /256；每个 CS 窗口严格收发 256 字节。
- 从机使用硬件 NSS，在 CS 高电平时准备 DMA；主机拉低 CS，保留至少 1 tick
  的 setup 时间后传输，完成后拉高 CS，帧间保留原有 2 ms 调度间隔。
- setup/帧间隔不是从机就绪保证。从机未及时装载时，主机收到的无效快照不交付应用，
  不推进接收序号、不确认发送，后续轮询重试。应用请求原有期限仍有效。
- 帧布局沿用 16 字节头、240 字节有效载荷、双 CRC16、seq/累计 ACK、HELLO。
  版本字节改为 2，拒绝与旧版 1 混用；没有新增虚假的“READY 引脚”。
- 有效空帧用于轮询与携带 ACK；接收侧背压时保持 ACK 不变，发送侧保留待确认数据。
  重传通过序号去重，不把旧帧再次交付应用。
- CS 提前释放的部分帧会清理 SPI/DMA，不与下一 CS 窗口拼接；超长窗口检测到
  RX 残留/溢出后丢弃。卡住的 CS、DMA 有界等待，不能永久占用事务。

## 编译与回退

H779 应用的 `platform_config/GD32H779V_START.config` 设置：

```text
CONFIG_ENABLE_GD32H77X_AT_TRANSPORT_SPI=y
CONFIG_ENABLE_GD32H77X_AT_SPI_PSC_256=y
CONFIG_ENABLE_GD32H77X_AT_SPI_CS_ONLY=y
```

独立边界探针：

```sh
.venv/bin/python platform/GD32H77X/gd32h77x_os/tools/build_project.py \
  platform/GD32H77X/gd32h77x_os/project/spi_at_cs_boundary_probe build \
  --chip GD32H779VW --board GD32H779V_START
```

VW553：

```sh
cd platform/GD32/gd32_os
VW553_GATEWAY_TRANSPORT=SPI VW553_GATEWAY_SPI_CS_ONLY=ON \
  VW553_GATEWAY_DIAGNOSTICS=ON ./cmake_build.sh vw553_at_gateway
```

从 0x08000000 烧录 `scripts/images/image-all.bin` 整包，不使用应用单体。
两端必须一起升级。回退时 H779 关闭 CS_ONLY、VW553 设置 CS_ONLY=OFF，
并恢复 READY 线；只刷一端不能通信。

主机测试：`sh platform/GD32H77X/tests/spi_at_integration/run.sh`，分别以 v1/v2
编译执行双向长度/地址矩阵、CRC、重传/去重、背压、版本拒收及从机 CS 边界检查。
主机测试不能代替实际 CS/DMA 时序、启动、配网、TLS 和长稳验证。

## 实机记录（2026-11-14）

两端已烧录并校验，H779 探针输出 `SPI protocol=2 cs_only=1`。
39 种长度 × 8 种发送/接收同步地址偏移 × 4 种数据模式，共 1248 项通过，0 失败；
采集到完整序号 1–1248，CRC32、逐字节回传、结束标记及两侧缓冲区哨兵均匹配。
这不是 64 种源/目标偏移交叉组合，也不是长稳验收。

结束后只读采样：DMA 完成 29028 帧，超时/错误均 0，READY 三项计数均 0；
有效帧 23606、无效快照 5422。无效快照已被拒收并通过后续轮询重传恢复，
不能描述为所有物理事务都成功或零 CRC 错误；尚未通过逻辑分析仪区分从机未及时装载、
事务时序及线路因素，仍需压力/吞吐与异常事务验证。

结果位于 `.cache/gino-validation/spi-cs-only-20261114/`：
`dma-boundary-result.json`、两端 `*-dma-boundary.log`、`cs-boundary-counters.log`。
随后已烧录并校验无 READY 版 switch_demo；Wi-Fi/TLS/MQTT 采集为 `*-switch.log`。

应用结果：自动连接已保存的 SST_Home，获得 192.168.50.32，约启动 11 秒时收到
MQTT_CONNECTED 及三个 SUBACK；MQTT TLS、HTTPS TLS 和设备 meta 上报成功。
未见本轮网络握手期间再次发生 AT 未完整帧空闲超时。既有启动 AT+RST/READY
交错导致的首轮初始化失败仍出现，异步恢复后网络初始化成功，此待办未关闭。
这次是保留绑定的自动恢复，不等于重新配网、纯云控制或长稳已通过。
