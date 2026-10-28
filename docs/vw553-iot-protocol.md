# VW553 IoT AT 最小扩展方案

本文依据现有 TuyaOpen AP/BLE 配网调用和 pinned VW553 SDK 源码，描述 H77 `switch_demo` 可行的最小 AT 集成范围。新增 `TY553X` 命令和事件均为**待评审草案，尚未实现、编译或烧录**。原厂 `CIP*`、`+IPD` 和 BLE 数据格式继续作为数据面协议；扩展只补齐 capability 标识、TCP/UDP 并行监听、TCP accept 通知和 Tuya GATT 服务/事件缺口。

## SDK 基线和源码证据

审查基线为 `.cache/gino-validation/vendor/GD32VW55x_RELEASE_V1.0.3g.zip`，SHA256：

```text
49995f9c482136e7b9be02fa4b1f1a6ea7716a215a0b993c1538a6d2f0792cea
```

下表源码路径均相对 ZIP 内 `GD32VW55x_RELEASE_V1.0.3g/`。现有 [atomic IPD 候选补丁](../../../.cache/gino-validation/vw553-ipd-candidate-20261019/vw553-uart-ipd-frame.patch) 在隔离副本上编译、打包成功，但未刷入模块；它只使 IPD 全帧 TX 原子，不提供能力标识、双 listener 或 BLE 事件扩展。

| 现有行为 | SDK 源码 |
| --- | --- |
| `CIPSEND` 先给 `>`，发送完成后 `SEND OK` 再 `OK`；失败为 `SEND FAIL` 再 `ERROR` | `MSDK/app/atcmd_tcpip.c:418,1518-1526` |
| TCP 数据行 `+IPD,<fd>,<length>: <binary>\r\nOK\r\n` | `MSDK/app/atcmd_tcpip.c:1676-1685` |
| UDP server 数据行 `+IPD,<peer-ip>:<peer-port>, <length>: <binary>\r\nOK\r\n`；没有 listener fd | `MSDK/app/atcmd_tcpip.c:1445-1460` |
| IPD header、payload、CRLF 和 `OK` 分别发送；UART semaphore 仅保护单次 send | `MSDK/app/atcmd_tcpip.c:1454-1459,1679-1685`; `MSDK/app/atcmd.c:1536-1547` |
| TCP accept 后 SDK 将 client fd 和 peer 信息存入 `cip_info.cli[]`，但没有向 UART 发 accept 通知 | `MSDK/app/atcmd_tcpip.c:1398-1444` |
| 仅有一个 `local_srv_fd/type/port`；第二个 server 被拒绝，接收任务只轮询该 server | `MSDK/app/atcmd_tcpip.c:1118-1121,1362-1460,2324-2375` |
| 原厂 AT 表已有 `CWMODE_CUR`、`CWJAP_CUR`、`CWSAP_CUR`、`CIPAP`、`CWLAP`；基础 STA/SoftAP 命令无需另造 | `MSDK/app/atcmd.c:781-787` |
| `CIPMODE=1` 需已有 passthrough fd，并限一个连接；这不是 H77 TLS/AP socket API 的替代实现 | `MSDK/app/atcmd_tcpip.c:2631-2661` |
| BLE payload `+BLEDATA:<conn>,<length>,<binary>\r\nOK\r\n` 当前由多次 UART send 输出 | `MSDK/ble/app/atcmd_ble.c:262-271` |
| `BLEGATTSSVC?` 只枚举已有服务；协议栈提供 `ble_gatts_svc_add()`、移除、读写确认与 notify API | `MSDK/ble/app/atcmd_ble.c:1779-1787`; `MSDK/blesw/src/export/ble_gatts.h:194-214,266-322` |
| 有服务表和 GATT callback 的 SDK 示例可直接参考 | `MSDK/ble/profile/datatrans/ble_datatrans_srv.c:120-205`; `MSDK/ble/profile/sample/ble_sample_srv.c:350-430` |

原厂普通固件没有 capability 命令，也没有 UART 多段数据原子保证。H77 在探针模式下只允许空闲 `AT`，需要在识别 capability 后才允许切换为 atomic-IPD wire mode。

## 最小能力协商与 UART 规则

新增只读查询暂定为 `AT+TY553XCAP?`，新命令注册前须检查完整原厂命令表无冲突。示例应答：

```text
AT+TY553XCAP?
+TY553XCAP:1,ATOMIC_IPD,DUAL_SERVER,TCP_ACCEPT,BLE_GATTS,ATOMIC_BLE_EVT
OK
```

版本 `1` 表示下表列出的最小扩展已实现。host 连接/复位后先发 `AT`，再查询 capability；旧固件返回 `ERROR`、超时、未知版本或缺少使用功能所需的 flag 时，保留 `PROBE_ONLY`，禁止调用相应 socket/BLE 能力。capability 查询本身不切换模块模式：确认 `ATOMIC_IPD` 后，H77 transaction client 才在本地选用与 `+IPD` 尾部 `OK` 匹配的 atomic wire mode；只有同时报告 `ATOMIC_BLE_EVT` 才可启用 BLE GATT 事件。不能以 `AT+GMR` 字符串代替行为能力声明。

最小 TX 修改是让 `at_hw_tx_sema` 覆盖整个异步帧的 header、payload 和尾部，并与所有命令应答、TCP/UDP 数据、BLE 事件共用；保留现有原厂终结符，不在事件外层再包另一套通用 envelope：

- TCP 数据仍为 `+IPD,<fd>,<length>: <binary>\r\nOK\r\n`。
- UDP 数据仍为 `+IPD,<peer-ip>:<peer-port>, <length>: <binary>\r\nOK\r\n`。
- BLE 普通数据仍为 `+BLEDATA:<conn>,<length>,<binary>\r\nOK\r\n`，整帧原子发送。
- 新增 accept、BLE GATT 操作事件使用有界文本头加长度字段，随后是可选二进制 payload 和 `\r\n`；每条事件独立持有 TX 锁，不追加 `OK`，避免混淆 AT 命令终态。

H77 RX task 持续运行，按长度识别 `+IPD`/`+BLEDATA`/新事件；在异步帧外才把 `OK`、`ERROR`、`>` 等交给当前唯一 AT 事务。坏 header、长度越界、UART overrun 或事务超时必须 fail closed：停止新命令，废弃当前帧和相关 fd；不能把未知尾部的字节重新解释成成功响应。清 RX/parser 后重探测；无法有界重同步时，走已验证 reset 路径重启 553。复位使全部 fd、service id 和 pending GATT token 失效。非幂等的 socket send、BLE write、service add 超时不得自动重放。

## TCP 与 UDP 双 listener

TuyaOpen `src/tuya_cloud_service/netcfg/ap_netcfg.c` 普通 AP 配网要同时开 UDP 广播 6667 与 TCP TLS-PSK 6668；pincode 分支 TCP 7001。TLS 在 H77 终止。原厂 `CIPSERVER` 单 listener 不满足这一需求。保留 `CIPSTART/CIPSEND/CIPCLOSE/CIPMUX` 的 client 命令和原厂数据格式，只新增一个按类型启停/查询 listener 的命令：

```text
AT+TY553XSERVER=UDP,6667    // 打开唯一 UDP listener
AT+TY553XSERVER=TCP,6668    // 打开唯一 TCP listener
AT+TY553XSERVER=TCP,7001    // 需要 pincode 时使用 TCP 7001
AT+TY553XSERVER=UDP,0       // 关闭 UDP listener
AT+TY553XSERVER=TCP,0       // 关闭 TCP listener
AT+TY553XSERVER?            // 返回各 listener 的 type、port、553 fd
```

最多同时运行一个 TCP listener 和一个 UDP listener；同类型重复启动明确报错，不替换已开 socket。成功响应返回真实 553 listener fd。SDK 接收任务必须将 listener 改为两项状态并在同一 `select()` 中轮询两个 listener 和全部 TCP client；启动、停止、查询、全关、CIPMUX 检查及清理路径都要同步更新，不能只扩展命令处理器。

TCP accept 后新增通知 `+TY553XCONN:<listener-fd>,<client-fd>,<peer-ip>,<peer-port>\r\n`。这是必要的新控制事件，因为原厂 accept 仅在模块内部存储 client，并不通知 H77；收到它后，H77 才能完成 `accept()` 并建立 Tuya fd 与 553 client fd 的映射。后续 TCP 数据仍用原厂 `+IPD,<client-fd>,<length>:`。

UDP server 继续用原厂 `+IPD,<peer-ip>:<peer-port>, <length>:` 格式。最小版本限制一个 UDP listener，因此 host 可将带 peer 的数据报路由至唯一 UDP server fd；`recvfrom()` 返回真实 peer IP/port，`sendto()` 使用该对端。TCP/UDP client 与 server 的 553 fd 必须单独映射，不能将模块 fd 直接暴露成 Tuya socket fd。

验收至少覆盖 TCP/UDP listener 并行、UDP 广播和 peer 回包、TCP accept/close 与 fd 复用、手机通过 SoftAP DHCP 获得租约，以及 H77 TLS-PSK 6668（pincode 7001）握手。STA/SoftAP 可复用原厂命令；`CWSAP_CUR`、`CIPAP` 和 DHCP 的真实配置顺序/事件语义仍须在目标固件实测，只有验证发现命令行为不足时才扩大固件改动范围。

## BLE 动态 GATT 的最小 AT 接口

AT 层新增 `ble_gatts_svc_add()` 的受限封装，不重写 BLE 协议栈。服务定义通过 prompt 后的有界二进制参数传入，避免 UUID/权限字段被文本解析器误读：

```text
AT+TY553XBLEADD=<request-id>,<payload-length>
>                // host 随后发送指定长度的 TLV
```

TLV v1 建议字段为：`format:u8 (=1), service_uuid_type:u8, service_uuid_len:u8 (2/4/16), service_uuid:bytes (LSB-first), start_handle:u16le, service_info:u8, attribute_count:u8`；随后重复 `attribute_count` 个记录：`uuid_type:u8, uuid_len:u8 (2/4/16), uuid:bytes (LSB-first), info:u16le, ext_info:u16le`。`service_info/info/ext_info` 按 SDK `ble_gatts_svc_add()` 和 `ble_gatt_attr_desc_t` 的 bitfield 语义验证。建议初版 payload 上限 1024 字节、attribute 数最多 32；需先按实际 GATT 表和 SDK heap 配置验证并通过 capability 报告实际上限。wire TLV 必须解析并转换为 SDK 预期的表结构，不可 cast 为 C struct；原始 descriptor 不包括 attribute 初值。

调用接受后响应 request id；SDK `BLE_SRV_EVT_SVC_ADD_RSP` 到达后发送 `+TY553XBLE:SVC,<request-id>,<status>,<svc-id>,<start-hdl>\r\n`。成功完成事件前 host 不可使用 svc id。删除用 `AT+TY553XBLEREM=<svc-id>`；只有收到 SDK remove response 后才释放映射。模块复位时所有动态 service id、handle 和 pending token 清零。

BLE read/write 等 GATT 事件用单一长度帧语法，具体字段固定且版本化：

```text
+TY553XBLE:<op>,<conn>,<svc>,<att>,<token>,<offset>,<length>:<payload>\r\n
```

`op` 至少含 `READ`、`WRITE`、`CCCD`、`CONN`、`MTU`；无关字段置 `-`。读请求包含 token/offset/max length；写事件包含 token/offset/value length 和 value；CCCD 包含 descriptor value；连接/断开包含连接索引及 reason；MTU 事件报告协商值。事件长度有明确上限，回调必须复制 SDK 临时数据至有界队列后返回，不能把 SDK 指针交给 UART task。完整事件由同一个 TX 锁一次发送；队列溢出显式报错并使 BLE provisioning 失败，不静默丢写入。

host 用 `AT+TY553XBLERESP=READ,<conn>,<token>,<status>,<total-len>,<value-len>` 加指定长度的 binary value 调用 `ble_gatts_svc_attr_read_cfm()`；write 用 `AT+TY553XBLERESP=WRITE,<conn>,<token>,<status>` 调用 `ble_gatts_svc_attr_write_cfm()`。同一 `(conn,token)` 只接受一次确认；按验证过的有限超时失败确认或断开连接，拒绝迟到/重复 token。notify/indicate 复用 `ble_gatts_ntf_ind_send()`，payload 长度不得超过协商 MTU 与 SDK 限制。服务添加/删除 response、连接、MTU、read/write/CCCD 事件按单 RX/TX 队列有序交付；disconnect 后不再向 host 提供该连接的新操作。

## 最小固件交付范围与验证顺序

真正需要改 553 的范围仅有：

1. TX 层提供持有同一 `at_hw_tx_sema` 的完整帧发送 helper；TCP/UDP `+IPD` 与 `+BLEDATA` 均改用它，并增加 `TY553XCAP?` 宣告支持的扩展版本和能力。保留原厂 `+IPD`/`+BLEDATA` header、长度、payload、CRLF、`OK` 语法。
2. TCP/IP AT server 状态扩为一 TCP 加一 UDP listener；接收 task 轮询两者；增加按 listener type 启停/查询命令和 TCP accept 通知。数据格式继续沿用原厂 `+IPD`，UDP 对端仍用原厂带 peer 的格式。
3. BLE AT 增加动态 service add/remove、read/write/CCCD 等必要事件和 confirm 命令；复用 SDK `ble_gatts_svc_add/rmv`、callback 和确认 API，保证所有 binary/event 输出整帧原子。

完整事件逐帧检查、长度校验和复位后清理是必须的。通用 epoch/seq/CRC 封装不是当前集成前置；若后续实测发现原子 TX 加长度帧仍无法诊断丢帧，可另提方案评审，不能把它叠加为本期必需协议。

执行顺序：先实现并 mock 验收 H77 AT parser/事务、fd 映射和 Tuya TKL 网络/BLE适配；同时在隔离 SDK 中实现并编译以上最小固件改动。host mock 和固件构建通过后，才需要将扩展固件刷入 553，验证真实 UART、STA/AP、socket 与 BLE 事件；最终由 Tuya 手机 App 完成 AP TLS-PSK/BLE 配网和云激活。当前真正外部条件是可访问的 Tuya 测试账号/网络、有效 PID/UUID/authkey、手机 App 和实体 553/H77 连接。没有这些条件只能完成源码/mock/固件构建验收，不能宣称真实配网通过。
