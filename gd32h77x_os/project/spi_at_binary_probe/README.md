# SPI AT 二进制回环探针

配合 VW553 的测试固件使用；普通固件不提供 `AT+TY553XLOOP`。

## 编译

仓库根目录运行：

```sh
.venv/bin/python platform/GD32H77X/gd32h77x_os/tools/build_project.py \
  platform/GD32H77X/gd32h77x_os/project/spi_at_binary_probe build \
  --chip GD32H779VW --board GD32H779V_START
```

VW553 从 `platform/GD32/gd32_os` 运行：

```sh
VW553_GATEWAY_TRANSPORT=SPI VW553_GATEWAY_DIAGNOSTICS=ON ./cmake_build.sh vw553_at_gateway
```

`VW553_GATEWAY_DIAGNOSTICS` 默认 OFF，正式/稳定固件不启用测试命令。

## 协议与用例

`AT+TY553XLOOP=<length>,<sequence>[,<receive_timeout_ms>]`，length 为 1–4096，sequence 为 0–65535。
可选接收期限为 1000–120000 ms，默认 10000 ms。553 先设置二进制接收目标，再发送单字节 `>`。
H779 随后发送严格 length 字节。响应依次为：

1. ASCII 行 `+TY553XLOOP:<sequence>,<length>,<CRC32十六进制>\r\n`。
2. 原样返回 length 字节二进制数据。
3. `\r\nOK\r\n`。

头、二进制体、结束标记在同一个原子 fragment 序列中发送；二进制体不经过字符串函数。
探针对响应序号、长度、双方 CRC32、逐字节内容和完整结束标记逐项检查。

矩阵为 1/239/240/241/1024/4096 字节 × 全零/全FF/特殊字节/可复现伪随机 × 5 轮，共 120 个常规用例。
特殊字节含 00、FF、CR、LF、A7、5C、OK、>、7E、7D。

默认不驱动 PC5。常规矩阵完成后，探针发送一个 4096 字节请求但只提供前 240 字节，然后打印
`BINARY_MANUAL_RESET_READY`，等待最多 110 秒，需此时手动复位 VW553。探针观察链路故障后清理旧接收状态，
发送新的序号并运行 10 个恢复用例。需同时采集 VW553 启动日志，确认链路故障确实来自该次硬件复位。
未操作复位时会输出 `BINARY_RESET_NOT_RUN`，不能作为复位通过证据。

## 实机结果（2026-11-13）

GD32H779V_START + LCKFB_GD32VW553：固件最终计数为 120 个正常用例、10 个恢复用例通过，失败为 0。
第 121 个请求只发送 240/4096 字节后，用户手动按 RESET；两端日志确认模块重启和 link fault。
未完成的第 121 个请求不算成功；第 122–131 个新序号的 4096 字节请求全部通过。
最终 `BINARY_RESULT PASS pass=130 fail=0`。序号、长度、CRC32、逐字节数据与结束标记均匹配。

原始日志、固件哈希及结构化结果在仓库 `.cache/gino-validation/spi-at-binary-20261113/`。
串口采集收到 118 条正常用例明细（缺少 99/100 两条），10 条恢复明细及完整的固件最终计数；
不能把缺少的串口明细当成 SPI 丢包或单独的逐行证据。
板上当前是专用诊断固件。未验证网络业务、吞吐调优、物理噪声注入或长时间稳定性。

主机检查可用 cc 编译 `platform/GD32/gd32_os/MSDK/vw553_at_gateway/tests/test_binary_loopback.c`，
并启用 AddressSanitizer/UndefinedBehaviorSanitizer 运行。
