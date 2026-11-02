# VW553 配网速度诊断与省电控制扩展

> **历史补丁/验证记录，不是当前构建入口。**
> 当前 VW553 AT gateway canonical 构建入口见
> [`platform/GD32/gd32_os/MSDK/vw553_at_gateway/docs/README.md`](../../GD32/gd32_os/MSDK/vw553_at_gateway/docs/README.md)。
> 下文中的旧 SDK、overlay/staging、`/tmp` 路径和 `patch -p*` 命令只用于复现历史实验，不参与当前构建。

## 现象

最近的 App AP 配网成功日志中，H77 在 `00:01:08` 发出
`AT+CWJAP_CUR="Pico","..."`，直到 `00:01:23` 才收到 `WIFI CONNECTED` 并取得 IP。
后面的 iotdns、激活 HTTPS 和 MQTT 都在数秒内完成，因此约 15 秒的主要耗时发生在
VW553 的 STA 建链内部。

VW553 管理层的连接策略是 3 次尝试、每次 EAPOL 超时 2 秒、重试间隔 2 秒。
同一套 H77 代码在 SST_Home 上曾于约 1 秒内完成建链，说明 H77 的 AT 编排、socket
和 TLS 不是主因；Pico 上表现为 WPA 4-way handshake 不稳定并在内部重试。

## 本次改动

### VW553 固件

新增 AT 扩展：

```text
AT+TY553XPS=<mode>,<dtim>
```

`mode` 为 0/1/2，分别对应关闭省电、常开和基于流量动态省电。`dtim` 范围是 0..10。
命令直接调用厂商 `wifi_netlink_ps_mode_set()` 和
`wifi_netlink_listen_interval_set()`。补丁位于 `vw553-speed-ps.patch`，应用在已有
CAP1/CAP2 扩展源码之上。

### H77 TKL

- `tkl_wifi_set_lp_mode()` 不再返回 `OPRT_NOT_SUPPORTED`，改为发送
  `AT+TY553XPS`。
- `tkl_wifi_station_connect()` 在 `CWJAP_CUR` 前关闭 553 省电，避免 WPA
  handshake、DHCP 和首次 HTTPS 被 DTIM 睡眠拖慢。
- 重复设置 STA 工作模式时不再重复发送 `AT+CWMODE_CUR=1`。
- 新增原生耗时日志：
  - `wifi lp disable before join status=`
  - `CWJAP transaction finished ... elapsed=`
  - `wifi station ready ... dhcp_wait= ... total=`

旧的原厂 AT 镜像没有 `AT+TY553XPS`，H77 会收到 ERROR 后继续建链，保持向后兼容。

### 日志降噪

`CONFIG_GD32H77X_VW553_VERBOSE_LOG` 默认关闭，不再逐条打印 AT 收发、TLS
分片、socket 数据和 payload preview。`VW553_SUMMARY` 仍保留少量必要日志：

- 建链耗时和结果
- DHCP 等待及总耗时
- AT 事务超时

switch_demo 的 TAL 日志级别从 `DEBUG` 调整为 `INFO`，保留错误、警告、notice 和
info，移除 HTTP/TLS/MQTT 的高频 debug 输出。需要排查协议细节时可临时重新打开
`CONFIG_GD32H77X_VW553_VERBOSE_LOG=y`。

STA 建链摘要额外打印 SSID、密码占位符、结果和耗时，例如：

```text
[VW553] wifi join start ssid="Pico" password=<redacted>
[VW553] CWJAP transaction finished ssid="Pico" status=0 elapsed=7633ms
```

socket/AT 仅在其失败时打印一条摘要；正常 TLS 数据收发不会产生逐包日志。

## 验证

- H77 host tests：13/13 通过（运行环境需要
  `ASAN_OPTIONS=detect_leaks=0`）。
- VW553 源码检查：`test_vw553_candidate_source.py` 通过，覆盖省电命令合法/非法参数。
- VW553 SDK 交叉编译通过，产物中同时存在 `AT+TY553XPS` 和 `AT+TY553XCAP`。
- H77 switch_demo ARM 构建通过，产物中包含三条新增耗时日志字符串。

### 固件产物

```text
.cache/gino-validation/vw553-prompt-race-fix-20261023/image-all.bin
.sha256: 249e3a3e70842cd6399af50776f7115d36808d2cbea88b3211e7257f1aea2ac1
.msdk:   a10efa76d002a9eb1b5708957f6c829924979ca04c6df25025a28d671c37aa8d
```

诊断版本在 `AT+TY553XSERVER` 失败时额外返回 `+TY553XERR:<reason>`，H77 会原样记录该行。
`+TY553XERR:4` 定位到 H77 发送的未加引号 `TCP` 被 553 错误地送去
`at_string_parse()`。修复后未加引号格式按文档直接解析，带引号格式继续兼容。
`test_vw553_candidate_source.py` 已加入真实 wire 格式 `AT+TY553XSERVER=TCP,6668`
的回归用例，并禁止自定义 AT 响应使用 553 `co_snprintf()` 不支持的 `%l/%z`
长度修饰符。server 成功响应固定为 `+TY553XSERVER2:TCP,<port>,<fd>,<generation>`，
其中 generation 使用 `%u` 数字输出。

UART `CIPSEND` 路径改为先启动 RX DMA，再向 H77 输出 `>` prompt，最后等待
payload 接收完成。原顺序是先发 prompt、再切 DMA，H77 收到 `>` 后立即发送的
前几个字节可能被普通 RX 中断消费，导致 553 永远等不满 payload。回归测试同时检查
TCP/UDP 发送函数必须使用 `at_hw_dma_receive_with_prompt()`，并验证 DMA 启动顺序。

```text
apps/tuya_cloud/switch_demo/dist/switch_demo_1.0.0/switch_demo_QIO_1.0.0.bin
.sha256: 02749de59ce17665d764278e8b4f536d5feef9755d9763e1cb999a4c517f97a5
```

当前只完成构建与静态检查，尚未在实板重新配网验证。烧录后需要重点确认
`wifi lp disable before join status=0`，并比较 `CWJAP transaction ... elapsed`。
如果仍在 Pico 上出现约 12~15 秒耗时，应抓取 553 原生日志，确认是定向扫描未命中还是
EAPOL 4-way handshake 超时，再决定是否启用固定 BSSID/channel 或 11bg 兼容模式。
