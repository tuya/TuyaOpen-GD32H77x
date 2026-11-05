# GD32H77X chip data

本目录只保存按 canonical DFP device 分组的平台数据。板级接线、烧录后端和
Console/VW553 绑定不在这里。

## 目录结构

```text
chips/
├── platform_chips.json                  # selector/alias -> canonical device
├── geometries.json                      # CNVM/ECNVM/AXISRAM/NOR 几何
└── <CHIP>/
    ├── <CHIP>.config                    # 平台 Kconfig 差分
    ├── partitions.csv                   # 芯片默认分区
    ├── flash_devices.csv                # 芯片默认内部设备表
    └── uart_isp.json                    # 可选且仅限已核实的 UART ISP 身份
```

- `<CHIP>.config` 只写相对 `platform/GD32H77X/Kconfig` 的差异。`GD32H779VW.config`
  当前保存已验证板级默认的 console/AT/烧录配置；真实板级差异才写入可选的
  `boards/GD32H77X/<BOARD>/platform_config/gd32h77x.config`。
- `flash_devices.csv` 必须引用该 chip 在 `platform_chips.json` 中批准的
  geometry。带 IROM2 的变体必须声明 `internal_ecnvm`；无 IROM2 的变体不得声明。
- `partitions.csv` 使用 `flash_devices.csv` 中的设备 ID。芯片默认布局只作为
  无板级覆盖时的 fallback，不代表具体产品板已经验证。
- `uart_isp.json` 只在 UART ROM-ISP 的完整 GETID 响应已单独核实时添加。当前
  `GD32H77DIW` 使用该文件，其他变体缺失时 UART ISP 会 fail closed。
- linker 模板由所有 chip 共享：`gd32h77x_os/components/startup/gd32h77x_linker.ld.in`。不要在每个 chip
  目录复制 `linker.ld`。

## 维护规则

1. 新 canonical device 先登记 `platform_chips.json` 和 `geometries.json`。
2. 需要芯片默认构建时，建立完整的 `<CHIP>/` 三件套；UART ISP 身份只在
   实板核实后增加。
3. `partial` 表示几何/算法资料已登记但未完成目标实板验收；不得把它写成
   `implemented`。
4. 修改 CSV 后运行：

```bash
./.venv/bin/python platform/GD32H77X/gd32h77x_os/tools/firmware/format_layout_csv.py \
  platform/GD32H77X/gd32h77x_os/chips/<CHIP>/partitions.csv \
  platform/GD32H77X/gd32h77x_os/chips/<CHIP>/flash_devices.csv
```

`gd32h77x_layout_csv_format` CTest 会检查本目录下所有分区和设备 CSV 的列对齐。
