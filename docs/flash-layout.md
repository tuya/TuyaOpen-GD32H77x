# GD32H77x Flash 分区与板级覆盖

**布局解析、生成、路由基础和烧录镜像绑定已实现。** GD32H779V_START 已把
Tuya KV 放到 ECNVM 尾部，并提供 AXI SRAM resident 的 4 KiB 页擦除与 8/32 B
编程路径；上板擦写、掉电、OTA 下载/安装/回滚和 bootloader 恢复仍需单独验收。
本文不把 host mock 或静态镜像检查当作硬件验证。

## 布局选择规则

平台提供 `GD32H77DIW` 片内默认布局，包含 OTA staging、升级状态和恢复策略的分区定义。分区入口使用两份简短 CSV：`partitions.csv` 描述逻辑分区到设备物理偏移的映射，`flash_devices.csv` 描述所选设备和 OSPI 接线；器件几何从平台 registry JSON 引用。构建使用现有 `OPEN_ROOT`、`PLATFORM_BOARD` 和 flash hook 收到的 `boards_root`，只检查 `boards_root/GD32H77X/<PLATFORM_BOARD>/partitions.csv`。board 分区文件存在时整份替换默认分区，**不按字段合并**；不存在时使用纯片内平台默认布局。board `flash_devices.csv` 仅在 board `partitions.csv` 存在时读取，并整份替换默认设备清单；缺失时只允许平台默认片内设备。任何存在但格式错误、引用断链或硬件不匹配的 CSV 都直接报错，不能静默回退。

例：

```text
platform/GD32H77X/gd32h77x_os/chips/GD32H77DIW/partitions.csv
platform/GD32H77X/gd32h77x_os/chips/GD32H77DIW/flash_devices.csv
platform/GD32H77X/tests/fixtures/boards/GD32H77X/GD32H77D_TEST_BOARD/platform_config/partitions.csv
platform/GD32H77X/tests/fixtures/boards/GD32H77X/GD32H77D_TEST_BOARD/platform_config/flash_devices.csv
```

上面是固定路径约定。平台用现有 `boards_root` 组成 board 路径，board 通过现有 `PLATFORM_BOARD` 参数选择对应目录，不猜测或扫描父目录。分区 CSV 列固定为 `name,device,offset,size,logical_addr`；`internal_flash` 和 `ospi0_flash` 是显式设备名，禁止根据地址推断存储介质。同名多行表示一个逻辑分区跨设备连续映射。设备 CSV 列固定为 `device,geometry,controller,cs,d0,d1,d2,d3`，geometry 引用平台 registry。Resolved manifest 的 `sourceInputs` 记录实际使用的两份 CSV，`sourceDigest` 绑定这些输入；flash hook 消费与 BIN 绑定的 resolved layout 和摘要。

## 平台默认布局

当前纯片内 fallback 的布局如下。所有地址区间均为半开区间 `[start,end)`；这是一份 OTA-capable 分区布局，不代表 OTA 软件流程已验收：

| 扇区 | 分区 | CNVM 地址 | 容量 | 用途 |
| --- | --- | --- | ---: | --- |
| S0 | boot | `[0x08000000,0x08010000)` | 64 KiB | 启动入口与 bootloader；boot 代码、常量及初始化数据装载映像须在 64 KiB Flash 预算内，栈/缓冲另计 RAM |
| S1–S13 | primary | `[0x08010000,0x080E0000)` | 832 KiB | 固定应用执行槽，向量表起点保持 `0x08010000` |
| S14–S26 | OTA staging | `[0x080E0000,0x081B0000)` | 832 KiB | 完整 app-only 更新包暂存 |
| S27 | OTA scratch | `[0x081B0000,0x081C0000)` | 64 KiB | 逐扇区安全交换的临时区 |
| S28–S29 | OTA metadata | `[0x081C0000,0x081E0000)` | 128 KiB | 双扇区状态 journal、事务和恢复状态 |
| S30–S31 | KV | `[0x081E0000,0x08200000)` | 128 KiB | Tuya KV 区域；KV 模块未启用，Flash 写入/擦除未实现 |

总量为 32×64 KiB=2 MiB。primary/staging 各 832 KiB。64 KiB bootloader 是容量预算，必须用最终 map 验证；还需验证 H77x NVMC 擦写期间的执行位置和 cache 约束，必要路径按原厂要求移至 RAM。应用容量须结合启用的功能和最终链接结果评估。

完整工厂镜像可以包含 boot 与 primary，不能覆盖 staging、scratch、metadata、KV；OTA 网络包只承载 app-only payload，不包含启动表、bootloader、启动间隙或 KV。有效 OTA payload 上限统一为：

```text
min(primary_payload_max,
    staging_size - slot_header - slot_metadata - alignment_padding)
```

不能把 staging 分区总字节数直接当成可下载 payload 上限。

## GD32H779VW ECNVM / KV 布局

GD32H77x 用户手册表 7-1 和本机 `gd32h77x_78x_nvmc.h` 给出 7.5 MiB ECNVM：
`Data ECNVM0`、`Bank1`、`Bank0` 位于 `0x08200000–0x088FFFFF`，独立
`Data ECNVM1` 位于 `0x08900000–0x0897FFFF`，大小 512 KiB。当前应用代码从
DECNVM0 开始，KV 使用最后的 DECNVM1，因此布局为：

| 分区/子区 | CPU 地址 | 大小 | runtime 几何 |
| --- | --- | ---: | --- |
| `ecnvm_code` | `[0x08200000,0x08900000)` | 7 MiB | SWD 64 KiB FlashDevice 擦除，32 B program |
| `kv` | `[0x08900000,0x08980000)` | 512 KiB | 4 KiB page erase，8 B 最小编程，32 B 突发事件 |
| `tuya_uf` | `0x08900000` | 256 KiB | 4 KiB |
| `tuya_kv_data` | `0x08940000` | 64 KiB | 4 KiB |
| `tuya_kv_key` | `0x08950000` | 4 KiB | 4 KiB |
| `tuya_kv_swap` | `0x08951000` | 16 KiB | 4 KiB |
| `tuya_user0` | `0x08955000` | 128 KiB | 4 KiB |
| `tuya_kv_protect` | `0x08975000` | 8 KiB | 4 KiB |
| reserved test | `0x08977000` | 4 KiB | 只允许 `ecnvm_kv_selftest run` |

`ecnvm_code` 的 SWD 烧录几何与 runtime KV 几何分开登记：SWD 使用 vendor
`ECNVM_7M_512K.FLM` 的 64 KiB sector，KV 使用 `NVMC_ECNVM_CTL.PER` 的 4 KiB
页擦除和 `NVMC_PERFCTL.ECNVM_PG_SZ` 的 8/32 B 编程。因此 `tos.py flash` 不会
把 4 KiB runtime 几何误当成代码烧录页，也不会越过 `0x08900000` 进入 KV。

擦除/编程期间，执行代码必须位于 AXI SRAM `0x24000000–0x240BFFFF`、关闭中断、
先复制源数据到该 AXISRAM，并在 BUSY 清零前不得访问 ECNVM。写入源范围由生成
布局宏锁定，ECNVM/CNVM、未映射地址或跨 AXISRAM 边界的指针全部拒绝。即使 KV
目标位于 DECNVM1，当前应用代码仍在前面的 ECNVM 区域，不能擦除正在取指的页，
也不能把 ECNVM 本身当作写入源。
这些是代码约束；真实擦除、回读、掉电和并发时序仍待硬件验收。

resolver 将两份 CSV 与器件 registry 归一化为 resolved manifest，并生成 `resolved_layout.json`、`gd32h77x_memory.ldinc`、`include/gd32h77x_memory_layout.h` 和 `gd32h77x_memory_layout.c`。生成表是 linker 与 C routing 的共享布局来源。

## 地址路由与上层 API

linker 和 startup 只使用真实 CPU 映射、可执行的物理 region；存储 logical address 是软件地址，不是 CPU 地址，不可 cast 成指针。需要逻辑地址时，由平台为 partition 定义明确、不冲突的 logical window，adapter 将 logical range 翻译为 `device + device_offset`。布局报告中保留 region/device 的物理 offset 和 CPU mapping（若存在）两种概念。

Tuya `tkl_flash_*` 使用 32 位 logical `addr`；`TUYA_FLASH_PARTITION_T` 用 `start_addr + offset` 描述分区。router 对完整 logical range 做溢出、分区边界、alignment 和所有 segment/backend 预检，再按 segment 拆分路由。因此单一 logical partition 可描述多个 device。当前 CNVM 读取以及布局绑定的 ECNVM KV 读/写/擦路由已接入；外部 NOR 和 OTA 操作仍不可用。任何路径都不把 logical window cast 为指针。`get_one_type_info` 只报告读写擦能力均已实现的 UF/KV 分区。Linker 永远不消费 logical window。

OTA staging、metadata、scratch 和 backup 分区目前只是布局声明。OTA TKL、下载、bootloader 安装、trial、回滚、掉电恢复、签名认证与 anti-rollback 均未实现或验证；分区存在不代表 OTA 更新能力已实现。

KV 几何同样要验证：当前 `tal_kv.c` 将 `block_size` 用作 littlefs 的 read/prog/block/cache size。CNVM 64 KiB erase 与 NOR 4 KiB erase 并不自动兼容现有 KV 配置。存在外部 NOR 也不代表 KV/FS 已可用。

## 构建、产物与烧录

平台 resolver 读取固定 default 或 board override，校验几何、物理范围、逻辑连续性、重叠、擦除兼容性、可执行映射和必需分区，并生成 linker 片段、C routing 表和 resolved manifest。构建产出的 `<project>_<version>.layout.json` 绑定来源类型/摘要、normalized layout digest、board/chip、ELF/BIN 名称及 SHA256、可烧录范围和镜像检查结果。flash bridge 每次烧录都重解当前布局、比对 report 与 ELF/BIN，并只接受 report 明示的允许写入范围；按 CNVM 64 KiB 擦除粒度补 `0xFF`，不会把未声明 payload 附进 BIN。

布局解析器使用现有 build parameters 和 boards root：

```bash
python3 platform/GD32H77X/gd32h77x_os/tools/firmware/resolve_memory_layout.py \
  --build-params <build_param.json> \
  --platform-root platform/GD32H77X \
  --boards-root boards \
  --out-dir /tmp/gd32h77x-resolved-layout
```

平台现有 `build_example.py` 将 `<project>_<version>.layout.json` 随 ELF/BIN 放入 `.build/bin/` 和 `dist/`，应扩展这份报告而非建立平行 manifest。优先在 GD32H77X platform hooks 实现，不改通用 `tos.py` CLI。平台 Kconfig 与板级 `gd32h77x.config` 只保留烧录后端选择与传输参数；DFU 写片内 CNVM full image，不会自动写外 NOR。外 NOR 工厂初始化需另一个独立 artifact/受控 loader，不得把 NOR 内容拼进 CNVM BIN。下载时必须核验构建 layout 与 ELF/BIN digest；覆盖 layout 错误不能静默 fallback。

DFU 后端不从 ROM 的 DfuSe 描述符推断擦除几何：H77x ROM 报告 4 KiB 页，而 NVMC 的 CNVM 是 64 KiB 扇区。原版 `dfu-util` 不作为支持 backend，因为平台不采用其基于 ROM geometry 的 erase path；bridge 使用原生 DfuSe 或 SWD。原生 DFU 还验证 protocol 2、DfuSe 1.1a 与下载/回读能力；USB 描述符只能确认 internal-flash alternate 和基地址，具体器件身份/容量仍由 `CHIP_CHOICE` 推导的 canonical target 与 build manifest 交叉核对。擦除地址、容量和器件身份都来自 resolver 输出的 CNVM 几何；Kconfig 选择后端并定义 VID/PID、可选 serial/path 和 alternate 条件。

器件身份对齐官方 DFP：`platform_chips.json` 登记的 16 个 canonical target 各有独立默认布局，`GD32H77D` alias 映射到 `GD32H77DIW`，当前仅保留芯片族兼容定义。registry key 使用器件名加官方 region，容量是独立的 `capacity_bytes` 字段。2 MiB CNVM 选择 `GD32H77x_78x_CNVM_2M.FLM`，1.625 MiB CNVM 选择 `GD32H77x_78x_CNVM_1M_640K.FLM`；运行时 bridge 校验本地官方 Pack 的 target、算法范围、擦除 sector 表和平台 registry。SWD 将 BIN 补齐 `0xFF` 到整 sector，用 `--format bin` 与显式基地址写入，并强制 sector erase、关闭自动解锁、保留未写数据、smart flash 和断连自动运行；默认逐字节比较完整 padded write window 的 readback。`CONFIG_FLASH_VERIFY=n` 是明确关闭回读验证的例外配置。pyOCD CLI 固定版本与可选依赖安装见 platform README。以上属于软件静态/pack 验证，不替代目标板烧录、启动和回读验收。

## 官方器件几何对照

以下数值取自 `GigaDevice.GD32H77x_78x_DFP.0.6.1.pack` 的 pdsc Devices/algorithm 表与原厂头文件（`SECTOR_SIZE=0x00010000`）。当前 registry 按每个 canonical target 保存独立设备行和默认 CSV 布局，不能从某个 W/P/I 料号推断另一型号容量。

| 器件（DFP Dname） | IROM1 CNVM | IROM2 ECNVM | CNVM Flash 算法 | ECNVM Flash 算法 |
| --- | ---: | ---: | --- | --- |
| `GD32H77DIW` | 2 MiB | 7.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_7M_512K.FLM` |
| `GD32H77DIP` | 2 MiB | 3.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_3M_512K.FLM` |
| `GD32H77DII` | 1.625 MiB | 无 | `GD32H77x_78x_CNVM_1M_640K.FLM` | — |
| `GD32H77EIW` | 2 MiB | 7.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_7M_512K.FLM` |
| `GD32H77EIP` | 2 MiB | 3.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_3M_512K.FLM` |
| `GD32H77EII` | 1.625 MiB | 无 | `GD32H77x_78x_CNVM_1M_640K.FLM` | — |
| `GD32H779IW` | 2 MiB | 7.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_7M_512K.FLM` |
| `GD32H779IP` | 2 MiB | 3.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_3M_512K.FLM` |
| `GD32H779II` | 1.625 MiB | 无 | `GD32H77x_78x_CNVM_1M_640K.FLM` | — |
| `GD32H779VW` | 2 MiB | 7.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_7M_512K.FLM` |
| `GD32H779VP` | 2 MiB | 3.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_3M_512K.FLM` |
| `GD32H779VI` | 1.625 MiB | 无 | `GD32H77x_78x_CNVM_1M_640K.FLM` | — |
| `GD32H779ZW` | 2 MiB | 7.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_7M_512K.FLM` |
| `GD32H779ZP` | 2 MiB | 3.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_3M_512K.FLM` |
| `GD32H779ZI` | 1.625 MiB | 无 | `GD32H77x_78x_CNVM_1M_640K.FLM` | — |
| `GD32H77RAP` | 2 MiB | 3.5 MiB | `GD32H77x_78x_CNVM_2M.FLM` | `GD32H77x_78x_ECNVM_3M_512K.FLM` |

订货号（如 `GD32H77DIWK7`、`GD32H77DIPK7`、`GD32H77DIIK7`）映射到上表器件；容量与算法由器件决定，不进名字后缀。

## 校验与迁移

resolver 对设备几何、容量和 segment 作结构校验；当 board override 声明未探测外 NOR 时，它只记录配置状态，并不替代硬件 probe。override 不存在时使用有效平台默认；override 存在但格式、芯片、范围或几何不符合时 fail closed。host 检查覆盖真实生成布局、跨 segment routing、越界、零长度、异构擦除语义、未实现 backend 与 bridge 的 stale/tampered artifact 拒绝。硬件 probe、NOR 读写、CNVM 写擦、掉电和 OTA 验证仍待后续单独完成。

CSV resolver 与 bridge 集成的 host 检查已通过：CTest 6/6，bridge unittest 17/17，DfuSe transport unittest 12/12，resolver unittest 19/19。测试通过隔离 boards root 验证实际 CSV resolver、分区 CSV 缺失回退、无效/缺少设备 CSV fail closed、未列入 `approved_geometries` 的模型被拒绝、缺失 `vendor_algorithm` 的片内几何被拒绝，以及 CSV 源变更后旧 report 被拒绝。CSV 阶段 sample project 增量构建、`verify_image.py` 静态布局校验和 4 项破坏性 ELF/BIN 输入检查均通过：归档 BIN 155968 字节、SHA256 `0f0cb6c159782f28ddd11ae59f9241ced06382142d38474ba14affeac5d21d9e`，resolved layout 摘要 `16953fe2…` 与 source 摘要 `a603f777…` 对应当前 `partitions.csv`/`flash_devices.csv`。迁移过程中发现 CSV 边界会丢 `ota_policy.backup`，已改为按 `ota_backup` 角色填充并补测试；修复后重建的 ELF/BIN 与归档产物逐字节一致。没有进行 USB 枚举或硬件写入。

当前已落地片内 OTA-capable platform default。H779 或其他容量需按具体型号定义布局，不能由 GD32H77D 料号推断。可发布布局必须包含 OTA 分区，但 OTA 软件路径仍须另行实现和验收。

布局定义现由 resolver 输出统一提供给 [GD32H77X shared linker template](../gd32h77x_os/components/startup/gd32h77x_linker.ld.in)、[镜像校验脚本](../gd32h77x_os/tools/firmware/verify_image.py)、[CNVM/KV adapter](../tuyaos/tuyaos_adapter/include/flash/gd32h77x_storage.h) 和 layout-bound flash bridge；CMake 根据 canonical target chip 选择 `gd32h77x_os/chips/` 中同一芯片的 `.config`、分区和设备表，并使用平台共享的 `gd32h77x_os/components/startup/gd32h77x_linker.ld.in`；共享模板缺失时明确失败。`gd32h77x_os/chips/` 是 TuyaOpen 平台自维护配置，不属于原厂 `gd32h77x_os/gd32` 或 `gd32h77x_os/components/FreeRTOS` 文件。当前平台板级构建支持 GD32H779V_START。烧录后端与 USB 身份由平台 `Kconfig`、芯片默认 fragment 和可选 `platform_config/gd32h77x.config` 管理，不使用 `flash.cfg`。
