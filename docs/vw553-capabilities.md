# VW553 首期无线适配清单

H77 使用 UART AT Socket，553 固件以已验证的 SDK 1.0.3g AT 工程为起点。两种配网的协议处理、TLS、涂鸦激活及 MQTT 在 H77 执行。该清单记录接口要求，不表示现有 AT 固件已经满足或通过验收。

## Wi-Fi AP 配网

主仓依据：`src/tuya_cloud_service/netcfg/ap_netcfg.c`。

| 能力 | 原厂参考入口 | 必须落实的接口语义 |
| --- | --- | --- |
| 工作模式 | `AT+CWMODE_CUR` | STA、SoftAP 与切换状态，上报真实完成事件 |
| SoftAP | `AT+CWSAP_CUR` | SSID、认证、信道、连接数，以及停止和重新进入配网 |
| AP 地址与 DHCP | `AT+CIPAP` 及原厂 DHCP 服务 | 根据上层指定的 IP/网关/掩码配置并提供地址租约；核对设置顺序及实际生效状态 |
| AP 扫描 | `AT+CWLAP` | 结果数组、SSID 长度、认证及信道/RSSI；核对 AP 活动期间扫描行为 |
| UDP 广播 | TCP/IP AT 命令 | 支持绑定 AP 地址和本地端口、广播目标及数据报边界 |
| TCP 监听与接入 | `AT+CIPSERVER`、`AT+CIPMUX` | `bind/listen/accept`，监听和客户端分别占用本地句柄，保留 553 descriptor 映射 |
| TLS 服务端 | 原始 Socket 数据 | H77 执行 Tuya TLS-PSK，支持非阻塞、超时、部分收发与关闭 |

当前主仓普通 AP 配网使用 UDP 6667 和 TCP 6668；pincode 分支使用 TCP 7001。平台传递主仓参数，不实现另一套手机配网协议或网页门户。

H77 Wi-Fi TKL 已实现 host-tested 的 STA/AP 基础 AT 命令适配，但尚未实板验收。原厂 tokenizer 按逗号切分双引号参数，当前 adapter 会拒绝含逗号的扫描筛选、SSID 或密码；扫描响应中的逗号 SSID 可显示，但不能通过该 AT parser 再连接。开放 AP 由 H77 发送空密码 `CWSAP_CUR` 参数；原厂 1.0.3g handler 对 `""` 返回 NULL 并拒绝，候选固件需修正此 handler 才能支持开放 AP。H77 的 AP MAC 预取依赖非并发 VIF 0；STA 静态 IP 命令按原厂 `ip, netmask, gateway` 顺序，AP `CIPAP` 按 `ip, gateway, netmask` 顺序发送。STA DHCP 连接只在轮询取得非零地址后上报连接成功。

## BLE 配网

主仓依据：`tools/porting/adapter/bluetooth/tkl_bluetooth.h` 及 `src/tuya_cloud_service/ble/`。

| 能力 | 当前 AT 线索 | 需要补齐或验证 |
| --- | --- | --- |
| 服务及特征 | `BLEGATTSSVC` 仅查询已有服务 | 动态注册涂鸦所需 UUID、属性、权限及 descriptor，建立 H77/553 handle 映射 |
| 广播与扫描应答 | `BLEADVSTART`、`BLEADVDATA`、`BLESCANRSPDATA` | 精确字节载荷、长度、广播参数和启动/停止状态 |
| 连接事件 | 原厂 BLE 事件回调 | 连接/断开原因、连接标识及事件顺序 |
| GATT 收发 | `BLEGATTSNTF`、属性值设置等命令 | 手机写入、通知及 CCCD 状态，区分二进制载荷和 AT 文本 |
| MTU | 原厂 BLE 协议栈能力 | MTU 查询/交换事件及上层分段策略 |

应按缺口扩展 553 命令与事件，而不是绕过 TuyaOpen BLE 配网协议。Wi-Fi 和 BLE 共用 UART 时，命令、Socket 数据及 BLE 事件需要可识别且不会互相截断的封装。

## UART 及 Socket 共用约束

- 事务层串行化命令，但接收层持续处理异步事件和二进制数据，不能持命令锁阻塞接收任务。
- SDK UART 接收路径的 `+IPD` 上报使用 553 socket descriptor，不能未经映射当作应用的连接 ID。`CIPSTART`、服务端接入和关闭事件须核对同一标识空间。
- parser 支持 descriptor TCP/connected-socket 数据和带 peer 的 UDP datagram 格式。CAP2 TCP accept 使用 `+TY553XCONN2`，close 使用 `+TY553XCLOSE`；H77 backend 独立映射 logical fd、553 `con_id` 和 lwIP fd，并用 generation/close ACK 管理复用。
- TCP/UDP 双 listener、UDP 特殊 `UDP_SEND_ID`、CAP2 状态快照和 retire ACK 依赖 553 端扩展。CAP2 候选已在 `CONFIG_ATCMD` 配置下编译，生产源码回归通过，归档说明见 `.cache/gino-validation/vw553-ty553x-cap2-candidate-20261022-1009/CANDIDATE-REPORT.md`。候选尚未烧录或实板验证，BLE 配网未实现；backend 仍须通过 CAP2 能力协商才启用网络。
- 连接关闭、553 复位、接收溢出或格式错误须使相关句柄和事务失效，唤醒等待者；不能把旧数据交给复用后的新连接。
- `select` 根据本地接收、接入、关闭和错误状态唤醒。TCP `recv` 返回 0 表示对端有序关闭，暂时无数据使用相应非阻塞或超时结果。
- BLE 和 AP 配网分别完成凭据获取、STA 联网及云激活，同时覆盖取消、错误凭据、超时、重新进入配网及共存。

当前 host 阶段已具备 CAP2 gating、状态查询、TCP/UDP socket backend、TCP accept/close 生命周期、UDP peer 收发、TKL wrapper、DNS IPv4 解析和 fake-AT 测试。最新平台 CTest 13 项通过，包含 Wi-Fi 响应解析及 DHCP 延迟/超时模型；switch_demo 的 GD32H77DIW ARM 构建通过。测试覆盖启动期间 early accept/IPD、UDP datagram final 分片、CIPSTART ERROR 状态分类、2920 字节 RX、超过 2048 字节的 TCP 分块发送、DNS 注入拒绝、原厂 Wi-Fi 响应格式，以及 DHCP 获得有效地址前不报告已连接。这些结果只证明 host 状态机、mock 契约和编译，不证明 553 或 H77 实物运行。

缓冲区有界：每 socket TCP stream ring 3072 字节；UDP datagram 最大 2920 字节、队列深度 2；TCP accept 队列深度 4；启动期间 raw-fd 暂存池 4 项、每项最多 3072 字节，最多保留 16 个 final 边界。缓冲、队列或暂存容量溢出会标记相关 socket 失败并返回 ENOBUFS，不会静默丢弃后继续路由。STATE 查询失败、格式不完整或状态未知时连接保留 quarantine，禁止 `con_id` 复用；只有确认 ABSENT 时释放 remote ID，RETIRED 需 ACK，ACTIVE 需完成关闭事件和 ACK。

仍未验收：553 CAP2 的实机响应、双监听并发、CIPSTART/close 的真实固件转换与 ACK 顺序、目标板 UART 行为、Wi-Fi AP/STA 和 TLS/云激活。CAP2 源码候选已编译并通过生产源码 host 回归，归档见 `.cache/gino-validation/vw553-ty553x-cap2-candidate-20261022-1009/`，但尚未烧录或实板验证，不能据此声称目标板网络可用。Wi-Fi 基础 AT 适配尚未经实板验证；DHCP 等待和断连监控只经过主机模型测试。BLE GATT 事件及 BLE 配网仍未实现并 fail closed；不能据 host mock 将未实现的无线流程标为可用。

当前 115200 8N1 链路要测量 TLS 配网、云连接与后续 OTA 的实际耗时，再决定是否提高波特率或更换传输方式。SPI 二层桥接不属于当前已确认的首期实现路线。
