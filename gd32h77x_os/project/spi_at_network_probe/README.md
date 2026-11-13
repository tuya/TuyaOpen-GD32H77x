# SPI AT 公网 TCP / UDP 验证

GD32H779V_START 通过 SPI AT 控制 VW553；设备连接 2.4 GHz Wi-Fi，电脑只负责烧录及串口日志，不运行局域网服务器。

构建：在仓库根目录运行：

```sh
.venv/bin/python platform/GD32H77X/gd32h77x_os/tools/build_project.py platform/GD32H77X/gd32h77x_os/project/spi_at_network_probe build --chip GD32H779VW --board GD32H779V_START
```

串口 115200，控制台采用轮询接收，自动输入时建议字符间隔 25 ms：

```text
net_join <SSID> <password>
net_status
net_public
net_cycle <SSID> <password>
```

凭据通过运行时命令传入，不编译到固件。控制台会回显输入，采集端必须在显示及保存之前过滤密码。

`net_public` 向 Cloudflare 公共 DNS `1.1.1.1:53` 发送 `example.com` A 查询，TCP、UDP 各 3 次。TCP 使用 DNS 长度前缀，完整收发；UDP 检查来源地址和端口。验证事务 ID、响应标志、返回码、问题字段及非零答案数。成功打印 `NET_RESULT pass=6 fail=0`。这是 DNS 协议响应校验，不是任意数据回显或压力测试。

网络验证在独立任务运行，栈为 4096 个 StackType_t；结束时记录剩余栈的历史最低值。PC5 不参与本测试，无需自动复位模块。

参考：https://developers.cloudflare.com/1.1.1.1/ 和 https://www.rfc-editor.org/rfc/rfc9210.html 。

`net_cycle` 首先入网并完成一轮公网 TCP/UDP 基线查询，然后执行 3 轮主动离网与重连。每轮确认 STA 已断开，检查离网前保留的 TCP/UDP socket 没有收到数据并关闭它们，再重新入网，创建新连接完成 6 次查询。离网期间的接收超时也算预期无数据，不代表旧 socket 已自动失效；此测试不要求自动恢复原有 TCP 连接。

验收输出为 `NET_CYCLE_RESULT pass=3 fail=0 completed=3`，各轮还需有 `NET_RESULT pass=6 fail=0`。旧 socket 关闭失败或重新创建失败都会计为失败。只覆盖 `AT+CWQAP` 主动离网，不覆盖热点断电或射频丢失。测试凭据只保留在运行时内存，任务结束清空。

PC5 复位异常暂缓处理，见 [联调待办](../../../docs/spi_at_todo.md)。

## 持续通信与跨帧响应

入网后执行 `net_soak 20`，轮数可选 1～100，默认 20。每轮分别用 TCP、UDP 查询 `example.com` A 和根域 DNSKEY（EDNS UDP 上限 1232 字节），共 4 次独立建立/查询/关闭。DNSKEY 响应必须大于 240 字节，验证跨 SPI payload 帧的接收；它不覆盖大请求发送。响应检查事务 ID、响应/截断标志、返回码、问题字段、非零答案数及 UDP 来源。

每轮记录空闲堆、历史最低堆余量及栈最低余量。连续 3 轮存在失败则提前停止；创建、查询或关闭失败均计入失败。20 轮成功应输出 `rounds=20 expected=20 pass=80 fail=0 large_tcp=20 large_udp=20`。堆统计用于观察回收趋势，不能单独证明完全无内存泄漏。该测试是短时重复通信，不等价于长时间稳定性或吞吐测试。

## 大请求发送

入网后执行 `net_tx`。按 RFC 7830 的 EDNS Padding（option code 12）构造 239、240、241、1024 字节的根域 DNSKEY 查询，Padding 全零，EDNS 接收上限 1232。TCP 使用正确的两字节 DNS 长度前缀及完整发送循环，UDP 要求一次发送返回完整请求长度；各大小、各协议重复三次，共 24 次。每次检查事务 ID、问题字段、响应标志、返回码、答案数和连接关闭结果，UDP 另查来源；响应须超过 240 字节。

验收输出为 `NET_TX_RESULT rounds=3 expected=3 pass=24 fail=0 large_tcp=12 large_udp=12`。DNS 服务不回显 Padding，因此本测试验证不同长度请求的提交与有效协议响应，不是网络端逐字节回显校验；任意二进制逐字节校验由先前的模块本地 loopback 覆盖。

协议参考：[RFC 7830](https://www.rfc-editor.org/rfc/rfc7830.html)。

## 多连接与 select

入网后执行 `net_multi`。每组同时打开 2 条 TCP、2 条 UDP，先用 `select` 验证 200 ms 空闲等待，再先发出所有连接的 DNSKEY 查询，使用 `select` 分发可读连接并按连接独立事务 ID 校验响应。前 3 轮每轮 4 条，随后主动关闭第一条 TCP，再用其余 3 条完成 2 轮，最后关闭全部连接。共 3 组，预期 54 次响应、12 次成功关闭、3 次空闲等待。

成功输出为 `NET_MULTI_RESULT pass=54 expected=54 fail=0 closed=12 idle=3 ...`。每组记录回收后堆余量与任务栈最低余量。这里验证多个同时存活的连接与交错收发，AT 命令仍由同一任务串行提交，不是多个调用任务同时发起 API 的线程并发测试。空闲超时验证不代表公网丢包恢复测试。

`net_multi_diag <mode>` 用于超时定位：0=混合大响应，1=双 UDP 大响应，2=双 UDP 小响应，3=混合小响应，4=单 UDP 大响应。每轮诊断记录 AT DATA 事件层按 raw fd 的字节/完整消息数，以及 SPI 帧、握手和硬件超时计数。计数用于与模块 UDP socket 日志对齐；不应单凭 SPI 错误计数为零认定公网丢包。

超时对照附加模式：5=双 UDP 大响应，每次发送后等待 1000 ms；6=双 UDP 大响应，目标改为 1.0.0.1。各模式会记录 `AT+CIPSTATE?` 返回的模块本地端口，便于将 UDP TX/RX 按连接与 DNS 事务对齐。应保留原条件复测，避免将偶发性变化误认为修复。

## 正式 TKL 网络适配接口实测

`net_tkl [resolver IPv4]`（默认 1.1.1.1）直接编译并调用平台 `tuyaos/tuyaos_adapter/src/system/tkl_network.c`，不复制其适配实现。原生工程通过构建目录中的配置桥接头将已生成的平台配置提供给 SDK 公共类型头，默认板级 transport 不变。此项不是整个 TuyaOpen 应用、TAL、TLS 或云连接验收。

测试包含 DNS 域名解析、IPv4 字符串转换、无效 fd 错误码映射，以及 TCP/UDP 各 239、240、241、1024 字节 Padding DNS 查询，重复三轮。各查询检查 TKL 超时设置、连接、空闲/可读 select、完整发送、有效响应和关闭；UDP 另检查非阻塞切换、空队列 EAGAIN 和来源地址。共 27 项，成功输出 `TKL_RESULT pass=27 expected=27 fail=0`。Wi-Fi 入网仍复用诊断控制台的 AT 入网命令，不将该项描述为 TKL Wi-Fi 接口验收。
