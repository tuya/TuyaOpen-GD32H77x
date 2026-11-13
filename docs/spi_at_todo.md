# SPI AT 联调待办

## 待办：PC5 / NRST 复位异常（用户决定暂缓，2026-11-13）

当前用户确认 PC5 已接回。一次复位验证捕获 H779、VW553 各 47 次启动；H779 reset flags 为 `0x04000000`，对应外部引脚复位。尝试通过调试器拉低 PC5，输入读回仍为高；随后 SWD 访问出现 RESET / FAULT ACK。因此尚不能证明 PC5 可以独立复位 VW553，也不能据此断定板子损坏或具体接错位置。

后续核对 H779 PC5（JP4-20）→ VW553 NRST 的实际端点、是否与 H779 NRST 意外相连，并做断线对照和电平测量。暂不继续脉冲测试，不将该项标记为已通过。

记录：`.cache/gino-validation/pc5-reset-20261113/result.json` 及同目录两端日志。

## 下一项：Wi-Fi 断线重连及 socket 重建

在 `spi_at_network_probe` 增加 `net_cycle <ssid> <password>`：基线公网 TCP/UDP DNS 检查后，执行三轮主动离网、确认 STA 已断开、旧 socket 接收与关闭、重新入网以及新 TCP/UDP 查询。密码仅运行时传入，日志采集端须过滤控制台回显。

只有实机出现 `NET_CYCLE_RESULT pass=3 fail=0 completed=3` 才算该项通过。若仍循环复位，保留构建结果，实机验证待设备稳定后执行。该项不覆盖 AP 断电、射频失联或长期压力测试。

### 断线重连实测结果（2026-11-13）

已烧录并通过读回校验；`net_cycle` 实机输出 `NET_CYCLE_RESULT pass=3 fail=0 completed=3 stack_free=3103`。三轮主动离网与重连均通过，六个旧 socket 关闭返回 0；基线及每轮重连后累计 TCP DNS 12/12、UDP DNS 12/12 通过。密码已从采集日志过滤。

记录：`.cache/gino-validation/spi-reconnect-20261113/result.json`，同目录 `ttyUSB0-cycle.log`、`ttyUSB1-cycle.log` 及 `flash.log`。

启动仍记录 `NET_PROBE_READY ... init=5`，传输恢复后完成上述测试；PC5 复位待办不因此关闭。尚未覆盖热点断电、射频失联和长期压力测试。

## 持续通信与大响应实测（2026-11-13）：偶发超时已记录，暂不深究

已完成 20 轮 80 次公网查询，汇总 `pass=79 fail=1 large_tcp=20 large_udp=20`。TCP / UDP 各 20 次 853 字节 DNSKEY 响应均通过跨 SPI 帧接收检查。每轮采样空闲堆均为 261312 字节，栈最低余量为 2847 个 StackType_t；本次采样未见堆持续下降，不等价于无泄漏证明或长稳通过。

失败项：第 2 轮 TCP A 查询。连接成功后 8000 ms 内接收字节数为 0，随后查询恢复。现有日志不能区分公网丢包、模块 TCP 栈与 AT/SPI 接收路径的原因，保留为待定位项，不通过重试抹去失败。

记录：`.cache/gino-validation/spi-soak-20261113/result.json`、`ttyUSB0-soak.log`、`ttyUSB1-soak.log` 和 `flash.log`。用户决定将该次偶发超时保留记录，暂不继续定位，不作为当前后续工作的阻塞项；若频繁复现再评估。尚未验证大网络请求发送、长时间稳定性和吞吐。

## 大请求发送实测（2026-11-13）：通过

`net_tx` 三轮共 24 次通过、0 次失败。TCP、UDP 均覆盖 239、240、241、1024 字节 Padding DNS 请求，收到 853 字节有效响应；发送完成检查、协议响应校验及连接关闭检查均通过。每轮结束空闲堆为 261312 字节，栈最低余量 2595 个 StackType_t。

记录：`.cache/gino-validation/spi-large-tx-20261113/result.json` 及同目录两端日志、构建和烧录日志。DNS Padding 不会逐字节回显，因此不将此项描述为远端逐字节数据校验；也不代表长稳或吞吐验收。既有 PC5 / 启动握手及上一轮偶发超时保留记录。

## 多连接实测（2026-11-13）：未全通过

每组 2 TCP + 2 UDP，三组计划 54 次响应检查。实际发起 34 次，31 次响应通过（TCP 16、UDP 15），3 次 UDP 等待超时；失败后停止当组余下轮次，共 20 项未执行，不能描述为 31/54 请求成功率。

第 1 组全部 18 项通过，关闭第一条 TCP 后其余连接继续收发，未发现响应串连接。第 2 组第 2 轮 UDP slots 2/3、第 3 组第 2 轮 UDP slot 2 在 select 等待窗口内未就绪。三组共 12 个 socket 关闭成功，3 次空闲 select 超时符合预期；每组回收后空闲堆 261312 字节，任务栈最低余量 2981 个 StackType_t。

现有日志无法区分公网丢包/服务限流、模块 UDP 栈与 AT/SPI 路由问题。此项与前一轮单次 TCP 超时分开记录，未标记多连接全面通过。

记录：`.cache/gino-validation/spi-multi-20261113/result.json`、两端 `*-multi.log`、`flash.log`。后续如需定位，可增加各连接收发计数并做双 UDP 连接单独对照。

### 多连接超时定位进展（2026-11-13）

H779 已加入 AT 事件层逐 raw socket 接收计数及 SPI 诊断计数，对照结果如下。失败后当组提前终止，expected 为原计划数，不能将未执行项算成失败：

| 模式 | 实际通过 | 实际失败 | 计划查询数 |
| --- | ---: | ---: | ---: |
| 双 UDP，大响应 | 21 | 1 | 30 |
| 双 UDP，小响应 | 21 | 1 | 30 |
| 单 UDP，大响应 | 15 | 0 | 15 |
| 双 TCP + 双 UDP，小响应 | 54 | 0 | 54 |
| 双 TCP + 双 UDP，大响应 | 14 | 6 | 54 |

各模式期间 SPI invalid、READY 超时及 SPI 硬件超时计数未增加（启动时已有 ready_hi=1）；未收到响应的 socket 在 AT DATA 事件计数中同样缺少对应数据报。因此未见 select 已有数据却不唤醒的证据，也不能简单归因于大响应分帧，因为双 UDP 小响应同样复现。现阶段仍不能区分公网/热点链路丢包与 VW553 UDP 接收路径。

已构建仅 DIAGNOSTICS=ON 时记录 UDP open/sendto/recvfrom 的 SPI 诊断镜像：`.cache/gino-validation/spi-timeout-diag-20261113/vw553_udp_trace_spi.bin`，SHA256 `4525972213335e0ef7c0da0271dce676ec01fb171c5c2dd698a06472dd7f7a43`。待用户使 VW553 进入 ROM 下载模式后烧录，利用事务 ID 对齐模块 UDP TX/RX 与 H779 接收记录。H779 已临时 halt，避免自动恢复干扰模块下载模式；完成烧录后需恢复运行。

对照记录：同目录 `comparisons.json`、`ttyUSB0-diag.log`、`ttyUSB1-diag.log`、`vw-build.log`。不能将“未见 SPI 错误”描述为已证明公网丢包。

### 模块侧逐包与网络条件对照结果（2026-11-13）

已烧录并校验 VW553 UDP trace 诊断固件，恢复两端运行。双 UDP 原条件出现 2 次超时：模块成功提交 50 次 UDP 发送，记录到 48 次对应 UDP 接收（这 50 次含后续混合连接组的 UDP 请求）；缺失 DNS ID 为 29990、30066，均为 raw fd 0，发送长度及 sendto 返回均为 28。对应两个连接的本地端口不同，未见端口冲突。两次缺包没有新增 SPI CRC/握手/硬件超时，也未进入 H779 AT DATA 事件层。

继续增加 AT+CIPSTATE 本地端口记录，并分别执行：同地址 1 秒发送间隔、备用 1.0.0.1 原速、原 1.1.1.1 原速。各组 30/30 通过，模块共 90 次 UDP TX 与 90 次对应 RX。原条件同样通过，因此不能声称减速或换地址已修复问题。

结论边界：已定位到缺失响应未被 VW553 UDP socket 接收，未见 SPI 转发或 H779 select 丢通知的证据。sendto 成功仅表示本地栈接受数据，不证明已在空口发出，也不能据此断言公网 DNS 限流；公网/热点、模块 Wi-Fi 驱动及 UDP/IP 接收前路径尚需抓包或更低层计数才能区分。未修改超时策略或添加重试掩盖失败。

完整记录与按 raw fd / 本地端口 / 远端 / DNS ID 对齐的分析：`.cache/gino-validation/spi-timeout-diag-20261113/result.json`。模块已退出下载模式，H779 已恢复运行，目前保留诊断固件。

## TKL 网络接口实测（2026-11-13）：已跑通，公网查询有超时

原生 SPI 诊断工程直接编译平台正式 `tkl_network.c`，未复制适配实现。DNS 解析、IPv4 转换、错误码映射均通过，实际执行了 timeout、connect、select、TCP/UDP send/recv、close；UDP 还覆盖阻塞/非阻塞切换及空接收队列 EAGAIN。查询长度覆盖 239、240、241、1024 字节，各协议三轮，每组 24 次查询加 3 项基础检查。

1.1.1.1：22/27 通过，5 次查询失败（1 TCP、4 UDP）；模块 UDP 日志中相应请求发送返回成功，但没有对应接收。保留此结果。1.0.0.1 对照：26/27 通过，1 次 TCP 等待超时，其余查询通过。两组结束堆余量均为 261320 字节，栈最低余量为 2769 个 StackType_t。不能将该项描述为全通过；未发现新的 TKL 适配缺陷，已有公网响应超时继续保留，按用户决定暂不深挖。

记录：`.cache/gino-validation/spi-tkl-20261113/result.json`、两组两端日志及构建/烧录日志。正式 TKL 网络实现已实际调用；完整 TuyaOpen 应用、TAL、TLS/HTTPS、云连接和 TKL Wi-Fi 接口尚未验收。板级默认 transport 未修改，当前板上保留诊断固件。
