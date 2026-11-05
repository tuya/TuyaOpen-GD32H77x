# TuyaOpen GD32H77x 平台

平台子仓库：`git@github.com:tuya/TuyaOpen-GD32H77x.git`。当前板级支持 `GD32H779V_START`，保留 GD32H77D/E/H779/RAP 芯片族定义供后续板卡使用。底层使用指定原厂 SDK，运行 FreeRTOS 11.2.0，不引入 RT-Thread SDK、设备框架或 SCons。

长期架构和板级接线分别见 [平台架构](docs/architecture.md) 与
[H779V-START/VW553 接线](docs/gd32h779v_start_vw553_wiring.md)。无需修改
VW553 时直接使用 [wireless_fw/vw553](wireless_fw/vw553/README.md) 中的稳定
预编译镜像，不依赖 `platform/GD32` 源码仓库。

## 当前状态

截至 2026 年 10 月 20 日，当前实现覆盖主仓构建集成、原厂启动与驱动依赖、FreeRTOS、日志、TKL 线程/内存/互斥锁/信号量/队列/时间、生成布局表驱动的 Flash 地址路由、硬件熵采集 helper、UART AT 接收解析器和独立事务状态机。布局 manifest 绑定 board/chip、布局摘要、ELF/BIN 名称和 digest；构建会把 ELF、CNVM 内部镜像、报告、ECNVM 及显式外部 payload 打进自包含 QIO 交付固件，flash bridge 烧录前重新解析当前布局并复核镜像。CNVM 有受限 DFU/SWD/UART ROM ISP 写入路径；GinoPi 外部 NOR 有基于 SRAM loader 的 SWD 写入路径。2026 年 10 月 20 日已在 GinoPi 实板确认 H77x ROM UART ISP：`/dev/ttyUSB1`、115200 8E1、GETID `44495764FFFFFFFF`，并通过 `tos.py flash` 对 CNVM 完成擦除、写入和逐字节回读。外部 OSPI 的配置字节与 pin record 已按厂商 CLI 默认路径准备，但仍待实板写入回读验收；不能据此认为 UART 外片路径已可用。布局绑定驱动的 GD25Q64E OSPI 运行时协议层现含 JEDEC、读取、页编程和 4 KiB 擦除，以及主机模型测试；这些 NOR 写擦 API 尚未接入 TKL Flash/backend，也尚未在目标板验证，调用不得视为硬件验收。CNVM KV 读写擦代码和主机测试已完成，但目标板 KV 写擦及缓存行为尚未验证；OTA 流程仍未实现。`tos.py build` 生成自包含 QIO 打包固件、ELF、MAP 和布局报告，并执行静态布局校验。历史设备运行和 DFU 状态见下方既有记录。

此前 H77 曾运行通用 `examples/system/os_thread/`。当时用户复位后，通过 H77 UART1 以 115200 8N1 捕获到 `GD32H77D GinoPi`、FreeRTOS 11.2.0、CPU/CPUID、`Project name: os_thread`、`example_task` 创建、5 次 worker 输出以及 TAL 线程删除路径。相邻 worker 行的 host monotonic 间隔为 2.0031–2.0037 秒；删除日志后继续监听 17.84 秒，没有新字节或 PANIC/HardFault。Raw 日志和逐行时间见[os_thread 历史运行证据](../../.cache/gino-validation/h77-os-thread-20261019T025510Z/runtime-verification.json)，raw 3424 字节 SHA256 为 `4378c49108660a983c6067b3cfd98c67aeb0df323335ce3dd57ec01e680dd36e`。这仅验证当时运行镜像的启动、worker 休眠周期和线程删除路径，不证明 heap 无泄漏、长稳或完整 OS/外设验收。此前 `sample_project` 首次启动记录见[历史运行证据](../../.cache/gino-validation/h77-standard-sample-20261019T023008Z/runtime-observation-20261019T024529Z/observation.json)。

此前运行的 CLI 固件通过 UART 命令验收：用户报告 9 条诊断结果全部 PASS，累计 62 项检查、0 失败，涵盖 `diag_all` 以及 `diag_info`、`diag_tick`、`diag_heap`、`diag_memory`、`diag_queue`、`diag_semaphore`、`diag_mutex`、`diag_thread`；另验证了 `diag_echo %` 与 `sys_heap`、`sys_thread`、`sys_version`、`sys_status`、`sys_tick`。串口记录见[CLI 验收归档](../../.cache/gino-validation/cli-acceptance-20261019/)。本轮重新写入同一 CLI 镜像并完成全片回读，但尚未复位确认本轮启动。通用 `examples/system/os_queue/` 仅离线构建，未烧录或运行。VW553 UART 传输层、AT parser/client 与当前固件的 `AT` 探针白名单有主机测试；传输层接收板级 UART 参数，使用 ISR stream buffer、串行事务和独立事件队列，尚未上板验证。原装 VW553 固件策略仍为 probe-only，只有普通 `AT` 可发送；异步事件回调不保证在事务结果返回前完成，因此尚不能用于同步网络查询或 Socket/Wi-Fi/BLE 操作。Socket 事务必须等原子 IPD 固件协议与响应关联设计完成后才能启用。`tkl_system_get_random()` 使用 GD32H77x NIST TRNG helper 并在错误时 fail-closed；switch_demo 启动期间曾报告 TRNG 失败，新的寄存器快照诊断尚待受控硬件运行读取，因此根因仍未确定，TRNG 也未完成实板验收。

首个功能验收版本必须同时支持 BLE 配网、Wi-Fi AP 配网、STA 联网和涂鸦云激活。当前 bring-up 配置关闭尚未实现的无线和外设 TKL，构建会明确拒绝启用这些能力；这只是分阶段启动配置，不改变功能验收范围。

当前 CNVM KV 的布局驱动读、写、擦代码及主机测试已具备，但尚未在目标板验证 KV 写擦和缓存行为；UF/KV_DATA 信息仅在对应读写擦 backend 全部可用时公布。其余未实现项包括 TKL OTA 下载/安装/回滚、TKL 远程 Socket、Wi-Fi 与 BLE TKL、两种配网、STA 联网、云激活、显示/触摸。UART ROM ISP 的外部 NOR 写入依赖当前 manifest 描述的设备绑定，以及板级提供并核实的逐设备 OSPI profile；包验证通过本身不会让任意布局设备获得写入能力。GinoPi 已核实 UART ROM ISP 的 GETID `44495764FFFFFFFF`，但 UART CNVM 完整读回尚待复验，也没有已核实的 UART OSPI profile，因此其 UART NOR 写入尚不可用。布局中存在 OTA staging 和 metadata 分区不代表 OTA 更新能力已实现。`tkl_system_get_random()` 使用硬件 TRNG helper 并 fail-closed；启动失败诊断会保留错误阶段和受限寄存器快照，不含随机数据，根因和实板TRNG验收仍待确认。根据 GinoPi 原理图中外部 VCORE 稳压器供电及 SMPS 电感未装配的证据，当前编译显式选择 `SEL_PMU_SMPS_MODE=PMU_BYPASS`；实板冷启动和核心电压仍未验证。

## 2026-10-19 回归记录

默认配置和 `GD32H77X_AT_PROBE=y` 配置均通过 `tos.py build`，两种构建生成的 ELF/BIN 均分别通过 4 项镜像检查。Host ASan/UBSan CTest 的 parser、AT client、Flash 边界和 entropy mock 共 4 项全部通过。基于 GinoPi 原理图，本轮构建定义 `SEL_PMU_SMPS_MODE=PMU_BYPASS`；回归结束时已将配置恢复为 `GD32H77X_AT_PROBE=n` 并重新构建。最终默认 BIN 为 117800 字节，SHA256 为 `5782e532ee4ed587b0db688d19d81c65617a6fdceffccd83b0de35dfecfa406e`。

本轮编译有主仓 `tuya_tools` 的既有 signedness 警告。主仓 39 项 Kconfig 端到端测试此前已通过，不属于本轮回归结果。离线构建和静态镜像检查之外，sample_project 与 os_thread 均已另行完成实板启动日志捕获；当前记录没有验证完整 OS/外设、冷启动、核心电压或 TRNG 硬件行为。

CSV 输入阶段完成独立 host CMake 检查 6/6、flash bridge unittest 17/17、DfuSe transport unittest 12/12、resolver unittest 19/19；临时 board roots 覆盖实际 resolver 与 stale/malformed/missing CSV 拒绝，registry 用 `approved_geometries` 决定某芯片可引用哪些几何。CSV 阶段的 sample project 构建、`verify_image.py` 静态布局校验和 4 项破坏性输入检查均通过，归档产物为 155968 字节 BIN，SHA256 `0f0cb6c159782f28ddd11ae59f9241ced06382142d38474ba14affeac5d21d9e`，layout 摘要 `16953fe2…`、source 摘要 `a603f777…` 与当前 CSV 输入一致；修复 `ota_policy.backup` 缺失后重建产物与归档逐字节一致。该阶段没有执行 USB 枚举、板上探测或写入。

同日追加一次平台诊断 CLI 构建：打开 `ENABLE_UART`、`ENABLE_SERIAL_CLI_CMD`、`GD32H77X_DIAG_CLI` 后 `tos.py build` 成功，BIN 为 156896 字节；`verify_image.py` 静态布局校验与 4 项破坏性输入检查通过，ELF 中确认存在 `diag_*`、`sys_*`、`help`、`tuya> ` 等命令字。随后经 USB DFU 写入前 3 个 64 KiB 扇区并完成 2 MiB 回读比对，写入区与补齐镜像一致、其余扇区与写前备份一致。串口验收已完成：`diag_all` 及 8 条独立诊断命令共 9 条结果全部 PASS（累计 62 项检查、0 失败），并验证 `diag_echo %`、`sys_heap`、`sys_thread`、`sys_version`、`sys_status` 和 `sys_tick` 可用。串口原始记录及逐命令解析保存在 `.cache/gino-validation/cli-acceptance-20261019/`。固件通过 `build.date`/`build.time` 内嵌 `__DATE__`/`__TIME__`，因此逐次构建的 BIN 摘要不同，不能用摘要判定构建是否一致。

## 原厂依赖来源

指定原厂归档：`GD32H77x_78x_Demo_Suites_V0.5.0.7z`。

```text
SHA256 0e15f0a2456ff8273c6cdd2008027a9a1152657fbc46754e41f1f04889783905
```

`gd32h77x_os/gd32/` 与 `gd32h77x_os/components/FreeRTOS/` 的初始内容从该归档提取。`import_sdk.py` 在导入时校验压缩包 SHA256；导入完成后，这些开源 vendor 文件允许按项目需要进行修改，Git 负责记录变更，不再通过逐文件 manifest 阻止 patch。平台自维护的 `gd32h77x_os/chips/` 芯片配置与 linker 模板同样属于平台代码。保留原厂许可及 FreeRTOS/CMSIS 许可证；本仓自行实现的代码采用 Apache License 2.0。

重新导入需要 Python 3.11 或以上及 libarchive：

```bash
python gd32h77x_os/tools/firmware/import_sdk.py /path/to/GD32H77x_78x_Demo_Suites_V0.5.0.7z
```

导入器只接受上述摘要。请使用原始压缩包，不从其他 BSP 或临时 SDK 工作副本混入依赖。

## 在 TuyaOpen 主仓构建

将本仓置于主仓 `platform/GD32H77X/`，主仓还需要相应的 `boards/GD32H77X/`、平台清单和 `examples/system/gd32h77x_bringup/` 集成。板级目录边界、必配/选配项及 Kconfig/config/CSV 形式见 [`boards/GD32H77X/README.md`](../../boards/GD32H77X/README.md)。

### 独立配置生成

TuyaOpen 和 GD32H77X 各自维护独立 Kconfig、配置生成器和头文件。TuyaOpen
通过 `build_param.json` 传递芯片、板型和布局身份，并通过 project context 把
已 staging 的 `GD32H77X_PROJECT_CONFIG` 传给平台通用配置解析器。

TuyaOpen 构建时，如果板级存在覆盖，平台前端先 staging 板级配置：

```text
boards/GD32H77X/<BOARD>/platform_config/gd32h77x.config
→ <build>/gd32h77x/project/tuya/project.config
```

当前 `GD32H779V_START` 不包含 `platform_config/`，因此不生成 project override，
直接使用 `GD32H779VW` 芯片默认配置。

然后 `platform_config.py` 对任意 project 使用同一套解析流程：

```text
platform/GD32H77X/Kconfig
→ gd32h77x_os/chips/<canonical_variant>/<canonical_variant>.config
→ project/<project>/project.config（可选）
→ <build>/generated/platform_config/gd32h77x_platform.config
→ <build>/generated/platform_config/gd32h77x_config.h
→ <build>/generated/platform_config/gd32h77x_config.cmake
```

standalone project 直接使用自身的 `project/<project>/project.config`。
`gd32h77x_os` 不发现 board，也不对 TuyaOpen project 做特殊判断。

平台源码包含 `gd32h77x_config.h`，CMake 包含 `gd32h77x_config.cmake`。
TuyaOpen 的 `tuya_kconfig.h` 只描述 TuyaOpen 自己的通用配置和板型身份。

```bash
# 在 TuyaOpen 主仓根目录执行
. ./export.sh
export GD32H77X_TOOLCHAIN=/opt/gcc-arm-none-eabi-10.3-2021.10
tos.py check
cd examples/system/gd32h77x_bringup
tos.py config set BOARD_CHOICE_GD32H77X=y BOARD_CHOICE_GD32H779V_START=y
tos.py build
```

已通过构建验证的工具链是 GNU Arm Embedded 10.3-2021.10，编译器版本 10.3.1；准备脚本检查该版本。未设置 `GD32H77X_TOOLCHAIN` 且 `PATH` 中没有 `arm-none-eabi-gcc` 时，`platform_prepare.py` 和 `toolchain_file.cmake` 会自动探测 `/opt/gcc-arm-none-eabi-*`。CPU/ABI 参数统一为 `-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=softfp`。原厂工程记录的 GCC 15.2.1 尚未验证。

FreeRTOS 使用原厂归档内的 `GCC/ARM_CM7/r0p1` port。该 port 包含早期内核修订的勘误处理，FreeRTOS 随包说明允许用于全部 M7 修订；首次运行日志打印 CPUID，供实板核对。任务优先级 0 保留给 idle，TKL 优先级映射为 FreeRTOS 优先级加 1；栈及水位值按 TKL 要求使用字节。有限等待向上换算 tick，`UINT32_MAX` 表示永久等待。

构建中间目录为应用的 `.build/gd32h77x/`，不同应用不会共用平台构建目录。最终文件位于 `dist/gd32h77x_bringup_1.0.0/`：

| 文件 | 用途 |
| --- | --- |
| `gd32h77x_bringup_QIO_1.0.0.bin` | TuyaOpen 完整 QIO 交付固件；包含 CNVM、ECNVM、ELF、报告和显式外部 payload。QIO 是交付类型名称，不表示 H77 使用 QSPI 启动 |
| `gd32h77x_bringup_1.0.0.elf` | 符号及调试信息 |
| `gd32h77x_bringup_1.0.0.map` | 链接布局及资源占用 |
| `gd32h77x_bringup_1.0.0.layout.json` | 所选 layout 来源/摘要、ELF/BIN digest、烧录范围及静态布局检查结果 |

### 通过 `tos.py flash` 烧录 H77D GinoPi

主仓 `tos.py flash` 默认使用 `.build/bin/<project>_QIO_<version>.bin`。GD32H77X flash bridge 按包内容识别 QIO 固件，自带报告、ELF、CNVM 内部镜像、ECNVM 和 payload，不依赖旁边的 sidecar 文件。包验证器用 `boards_root` 重新解析当前布局，并比较 source/layout digest、板型、芯片、分区路由和镜像摘要；缺失或过期内容直接拒绝。旧 raw BIN 仅作为受控兼容路径。CNVM 写入范围始终限于 boot+primary，H77x 扇区为 64 KiB，临时镜像按扇区补 `0xFF`；不会把 OTA staging、metadata 或 KV 自动拼入镜像。

如需在一次构建中显式选择片外分区 payload，在应用根目录创建 `flash_payloads.csv`，列头必须是 `partition,file`，每行选择一个分区，文件路径相对 CSV 所在目录。例如：

```csv
partition,file
filesystem,images/filesystem.bin
```

CSV 是否存在就是唯一开关：存在则校验并包含其选中的 payload，不存在则生成纯片内包。该行为不再由平台 Kconfig 控制。保护分区不会自动加入包，boot/primary 不能通过 CSV 覆盖。GinoPi 外 NOR 的 SWD 写入流程已实现：先复核 layout、pack、loader、同一 probe 与 NOR 只读身份/保护状态，再逐分区擦写并回读；这条外片路径尚未实板验收。通用 `uart` backend 可写入 manifest 与逐设备 profile 均通过校验的外部设备；当前 GinoPi 尚无相应 UART profile，因此不能据此宣称它的 UART 外 NOR 已可用。外片写入完成后才写 CNVM boot+primary；中间失败会停止后续阶段，不承诺跨器件回滚。DFU 后端在枚举设备或擦写 CNVM 前拒绝包含外部 payload 的包。`tos.py flash --no-verify` 时会明确报告回读已禁用。

SWD 外片写入当前只支持 GD25Q64E（布局器件型号 `GD25Q64ESIG`），以 GPIO 模拟单线 SPI，不启用 Quad/QE，也不依赖运行时 TKL NOR 驱动。loader 的 MCU canonical target 与 GPIO 接线均来自 resolved manifest；当前写入流程一次只处理一个受支持 NOR 设备。主机还须有 ARM GCC 与配套 `arm-none-eabi-objcopy`，bridge 会即时编译 SRAM loader；仅安装 pyOCD 不足以使用此后端。单独构建时必须显式提供生成的 layout manifest，由它选取 target 和该 NOR 的 `board_binding`：

```bash
ARM_NONE_EABI_GCC="${ARM_NONE_EABI_GCC:-${GD32H77X_TOOLCHAIN:+$GD32H77X_TOOLCHAIN/bin/}arm-none-eabi-gcc}"
LOADER_OUT="$(mktemp -d /tmp/gd32h77x-ospi-loader.XXXXXX)"
LAYOUT=examples/system/gd32h77x_bringup/.build/bin/gd32h77x_bringup_1.0.0.layout.json
"$OPEN_SDK_PYTHON" platform/GD32H77X/gd32h77x_os/tools/download/ospi_loader/build_loader.py \
  --compiler "$ARM_NONE_EABI_GCC" --out-dir "$LOADER_OUT" --layout "$LAYOUT"
```

`GD32H77X_TOOLCHAIN` 指向 GNU Arm Embedded 安装目录时，上述命令会从其 `bin/` 目录选 GCC；也可直接设置 `ARM_NONE_EABI_GCC`，或让 `arm-none-eabi-gcc` 与 `arm-none-eabi-objcopy` 位于 `PATH`。烧录时使用同一环境变量供即时构建使用。

TuyaOpen 默认 backend 可来自芯片配置或可选的板级 `platform_config/gd32h77x.config`；板级存在覆盖时由 TuyaOpen 前端 staging 为 build 下的 `project/tuya/project.config`。设备始终自动枚举；SWD Pack、UART baud 和 readback verify 是主机运行时参数。

| 项目 | 默认/来源 | 运行时覆盖 |
| --- | --- | --- |
| backend | `ENABLE_GD32H77X_FLASH_BACKEND_*` choice | 修改 Tuya project config 的来源文件 |
| SWD probe / UART port / DFU USB | 自动枚举 | `tos.py flash -p` |
| SWD Pack | 平台仓库内置 `gd32h77x_os/tools/packs/GigaDevice.GD32H77x_78x_DFP.0.6.1.pack` | `GD32H77X_DFP_PACK` 仅用于开发覆盖 |
| UART baud | `auto` | `tos.py flash -b` |
| readback verify | 开启 | `tos.py flash --no-verify` |

各支持的后端消费同一份布局与几何：

| 后端 | 说明 |
| --- | --- |
| `dfu` | 原生 DfuSe，经 ctypes 调用 libusb-1.0，不依赖外部 `dfu-util`，也不采用 ROM 报告的 DfuSe 描述符几何 |
| `swd` | 原厂 CMSIS-Pack Flash 算法，经固定版本 pyOCD 编程；校验官方 pack/target/算法几何，sector erase 后回读含 `0xFF` padding 的完整写入扇区 |
| `uart`（可选） | H77x ROM system-memory bootloader，经串口捕获 ROM 响应并保持该波特率；先写显式 OSPI payload，再写 CNVM，不依赖 SWD、pyOCD 或 CMSIS-Pack。当前 `GD32H779V_START` 默认使用 `swd` |

原版 `dfu-util` 后端已禁用：ROM descriptor 报告 4 KiB unit，而 H77 CNVM registry 使用 64 KiB sector；原工具在该 ROM 上的擦除行为未作为本平台可信路径。使用原生 `dfu`、`swd` 或 `uart`。原生 DFU 只接受 DFU-mode protocol 2 alternate、DfuSe 1.1a、支持 download 且默认回读时支持 upload 的 alternate，并检查 alt 名称仍标识从 `0x08000000` 开始的 internal flash。此信息能确认内部 flash bank，不能自动探测具体料号或容量；bridge 从 `CHIP_CHOICE` 对应的 canonical chip 与 build manifest 交叉核对目标身份，VID/PID、USB serial、端口路径和 alt 均只是可选筛选条件。

### GD32H779V START 的 CNVM + ECNVM 双区执行

`GD32H779V_START` 声明 7 MiB `ecnvm_code` 分区和末尾 512 KiB `kv` 分区。只要板级 `partitions.csv` 声明 `ecnvm_code`，平台就自动把应用 `.text/.rodata` 链接到 `0x08200000` 起的 ECNVM；boot table、向量表和 RAM 初始化数据装载段仍保留在 CNVM，KV 位于独立的 Data ECNVM1 `[0x08900000,0x08980000)`。QIO 固件同时携带 CNVM 启动镜像和独立 ECNVM 应用 payload，SWD bridge 只按 ECNVM 代码区扇区擦写并强制回读，再写 CNVM；不会写 KV。该布局必须使用完整 QIO 固件，raw BIN 和非 SWD 后端会被拒绝；`tos.py flash --no-verify` 不会关闭 ECNVM 回读。

2026-10-29 已在 H779VW 实板验证：当前镜像 CNVM 71,276 字节、ECNVM 433,652 字节，reset entry 为 `0x08200b45`，设备从 ECNVM 启动。ECNVM KV runtime 4 KiB page erase、8 B 最小/32 B burst program、AXI SRAM resident 执行和回读路径已实现；`ecnvm_kv_selftest run` 在 `0x08977000` reserved 页完成擦除、32 B 编程、回读比较/CRC 和最终擦除并 PASS。通过 `tal_cli auth` 写入 ECNVM KV 后，复位前后两次 `auth-read` 均读取成功。OTA 下载、安装、回滚仍单独保持未实现，不能据此宣称在线升级或完整网络验收已可用。

平台的器件身份来自 `gd32h77x_os/chips/platform_chips.json`，共支持 D/E/779/RAP 的 16 个 canonical DFP target。`GD32H77D` 是兼容 alias，映射到 `GD32H77DIW`；当前不再提供 GinoPi board，其他实际板卡应使用具体料号或明确维护自己的板级映射。Flash registry 的容量、64 KiB sector 和算法名必须与本地原厂 pack 一致。2 MiB 使用 `GD32H77x_78x_CNVM_2M.FLM`，1.625 MiB 使用 `GD32H77x_78x_CNVM_1M_640K.FLM`。SWD target 不能覆盖 layout/manifest 的 canonical target。

SWD 可选依赖固定在 `requirements-swd.txt`，通过 SDK Python 安装：

```bash
UV_CACHE_DIR=/tmp/tuyaopen-uv-cache ./.tools/uv/0.11.18/uv pip install \
  --python "$OPEN_SDK_PYTHON" -r platform/GD32H77X/requirements-swd.txt
```

`export.sh` 后续执行 `uv sync` 可能移除额外安装的可选包；若 pyOCD 不存在或版本不符，重新执行此安装命令。烧录步骤不会自动下载或安装依赖。

已批准的 `GigaDevice.GD32H77x_78x_DFP.0.6.1.pack` 直接存放在 `gd32h77x_os/tools/packs/`，全新 checkout 不依赖网络下载或机器级环境变量。`platform_prepare.py` 会校验固定 SHA256 `94ed659f9adb58c881114858e9a13dba4a0540765c4d9e4d305a24e13ca5dee6`，并按需安装固定版本 SWD Python 依赖；bridge 仍要求 pyOCD 0.46.0 并校验 PDSC 身份、版本和 Flash 算法。`GD32H77X_DFP_PACK` 仅作为开发期覆盖。仓库开发环境已用该 pack 和 pyOCD 对 16 个 target 做过无硬件算法/几何校验；尚未据此宣称 16 个型号或 Gino SWD 已实板验收。

如果选中板级配置使用 DFU 后端，让目标进入 USB DFU 模式后在应用目录执行：

```bash
tos.py flash
# 同时有多个匹配 DFU 设备时，用 USB 总线端口路径精确选择：
tos.py flash -p 3-3.2
# SWD 时 -p 传 pyOCD probe unique ID（可使用完整或唯一子串）
tos.py flash -p cmsis-dap:12345678
```

bridge 在 USB/SWD 操作前校验 layout report、ELF/BIN digest、当前 CSV 来源及 allowwrite 范围。DFU 按 VID/PID 和可选 serial/path 筛选，再确认 DFU mode、协议版本与内部 flash alternate；多设备时 TTY 可选，非 TTY 必须用 `-p` 指定 USB 路径。SWD 的 `-p` 是 pyOCD probe ID，非 TTY 多 probe 同样拒绝歧义；TTY 才允许明确选择。SWD 使用隔离临时目录、`--no-config` 与空 user script，强制 sector erase、`auto_unlock=false`、`keep_unwritten=false`、`smart_flash=false`、断开不自动运行，且成功写入后保持复位/停止状态。默认 readback 对完整 FF 填充后的扇区做逐字节比较；`tos.py flash --no-verify` 可在主机侧关闭回读。原生 `dfu` 默认同样回读 padded bytes；成功后仍按板级流程手动复位/退出 ROM。

#### UART ROM ISP（不依赖 SWD/pyOCD）

板级选择 `CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_UART=y` 后，bridge 改用 H77x ROM system-memory bootloader：主机使用串口和 BOOT 复位时序，不枚举 USB、不启动 pyOCD、也不读取 CMSIS-Pack。自动波特率捕获 ROM 响应后保持该波特率；不承诺无需复位即可切换到更高速率。缺少 `chips/<canonical_chip>/uart_isp.json` 中的 `expected_id` 时会在串口枚举前拒绝运行；值必须是经目标板单独核实的完整原始 GETID 响应。该路径可用于不依赖 SWD 的烧录，但身份绑定本身不代表所有芯片变体都已验证。擦除、写入和回读每 64 KiB（擦除每 1 MiB）输出进度、吞吐与 ETA。

```bash
# 在应用目录内，先按上节完成构建并生成完整 QIO
tos.py flash -p /dev/ttyUSB0
```

可用只读探针查看候选串口上的握手和 `GETID`（不会擦除或写入）：

```bash
./.venv/bin/python platform/GD32H77X/gd32h77x_os/tools/diagnostics/gd32h77x_uart_probe.py \
  -p /dev/ttyUSB0 --timeout 30
```

- 未指定 `-p` 时，工具枚举串口：单个候选自动选择；多个候选在 TTY 中列出编号供选择，非 TTY 模式要求显式指定 `-p`。指定端口时不会枚举其他串口。
- ROM OSPI 可直接控制的 NOR：写序为「先 OSPI 外片 payload，后 CNVM boot+primary」。ROM OSPI 阶段失败时不会写 CNVM。
- `GD25Q64ESIG`：ROM 无法用硬编码 JEDEC 初始化该器件，使用单设备 AXI SRAM UART loader。该 loader 会接管 CPU/UART，因此写序只能是「先 CNVM，后 OSPI loader」；loader 阶段失败时 CNVM 已写入，不承诺跨器件回滚。
- 两条外片路径都会在打开串口和写 CNVM 前完成 payload、设备兼容性和必要 profile 的预检；loader 二进制也会预编译，避免写到一半才发现工具链或构建错误。
- 外片 payload 来自构建时的 `flash_payloads.csv` 并随 QIO 固件分发；没有外部 payload 的包只写 CNVM。
- UART baud 默认 `auto`；`tos.py flash -b <integer>` 覆盖。
- ROM OSPI 路径要求板级 `ospi_profiles.csv`，列头为 `device,config,pins`，并逐设备校验。`GD25Q64ESIG` loader 路径不使用该 profile，而是使用 resolved layout 中的板级 OSPI binding pins，并校验只包含一个 `GD25Q64ESIG` 设备。
- UART erase mode 固定为 ROM ISP 的 `extended` 模式。
- 默认回读逐字节比较 OSPI 分区和 CNVM 镜像；`tos.py flash --no-verify` 可关闭验证。写入时间随 ROM 捕获或显式波特率变化。

2026 年 10 月 20 日曾记录 GinoPi UART CNVM 烧录与回读通过；后续复验未能重复完成 ROM READ。921600 8E1 的握手和 GETID 成功，164348 字节镜像写入完成，但读回校验超时；115200 只读复验取得 32 KiB 后也超时；最近一次 9600 尝试在握手阶段无响应，未进入 READ。这些结果不能证明物理波特率不可靠，仍需排查 ROM READ 协议、主机接收处理和超时设置。UART CNVM 验收暂缓，直到完整 boot+primary 写入与补齐扇区读回均通过。UART 外部 NOR 也未验收：还需完成 SRAM loader 的写入、擦除、读回和保护范围检查。不得据此宣称 UART NOR 已通过实板验收。

### 烧录工具离线复验

2026 年 10 月 20 日的无硬件回归共 82 项：layout 21、DFU transport 19、bridge 26、SWD backend 9、官方 pack/pyOCD targets 5、variant integration 2（其中遍历全部 16 个 canonical target）。Pack 测试默认使用平台仓库内置的 DFP，不需要设置环境变量。DFU 使用内存设备模拟，SWD probe/program/readback 均 mock；pyOCD target 列表查询不枚举 probe。本轮没有 USB 访问或硬件写入。

```bash
PY="$OPEN_SDK_PYTHON"
"$PY" platform/GD32H77X/tests/test_memory_layout.py
"$PY" platform/GD32H77X/tests/test_dfuse.py
"$PY" platform/GD32H77X/tests/test_flash_bridge.py
"$PY" platform/GD32H77X/tests/test_swd_backend.py
"$PY" platform/GD32H77X/tests/test_swd_pack.py
"$PY" platform/GD32H77X/tests/test_flash_variants.py
```

2026 年 10 月 20 日的烧录包增量复验另通过 104 项：CLI artifact hook 23、flash bridge 42、UART ROM ISP 20、package 11、OSPI writer 8（此前记录为 78 项，本轮补入 UART ISP 主机测试并重跑 bridge）。UART ROM ISP 测试用协议模型覆盖握手、GETID、自动波特率回退、CNVM 与 OSPI 擦写、地址映射、回读失败和 OSPI 失败不触碰 CNVM；不连接串口。bridge 测试包含由 `tos.py build` 自动生成的真实 Gino QIO 打包固件：将包单独放入空目录，使用真实 package validator/current resolver，再由 DfuSe 内存模型完成写入与读回；OSPI 计划测试验证真实打包 payload、分区边界、扇区计划并实际编译 SRAM loader，还用 8 MiB NOR 协议模型覆盖实际擦写命令循环、读回、超时及失败阶段。只 mock SWD target边界；没有连接 USB/probe，也没有写入 CNVM 或 NOR。旧的 82 项汇总是此前代码阶段的记录，与本段增量数字分开统计。

```bash
PY="${OPEN_SDK_PYTHON:-./.venv/bin/python}"
"$PY" -m unittest discover -s tools/cli_command/tests -t . -p 'test_cli_flash.py'
"$PY" platform/GD32H77X/tests/test_flash_bridge.py
"$PY" platform/GD32H77X/tests/test_uart_isp.py
"$PY" platform/GD32H77X/tests/test_flash_package.py
"$PY" platform/GD32H77X/tests/test_ospi_writer.py
```

## 启动与内存布局

| 区域 | 地址与大小 | 说明 |
| --- | --- | --- |
| 启动扇区 | `0x08000000`，64 KiB | 前 8 字节保存初始 SP 和 Thumb 复位入口，其余填 `0xFF` |
| 默认 fallback 的 primary | `0x08010000` 起，832 KiB | 向量表位于起点；实际链接容量由当前 selected layout 生成 |
| KV 预留 | `0x081E0000` 至 `0x081FFFFF`，128 KiB | 两个 64 KiB 扇区，当前未开放写入接口 |
| AXI SRAM | `0x24000000`，768 KiB | 启动阶段初始化 ECC；FreeRTOS heap 384 KiB，独立 libc arena 16 KiB，MSP 栈 8 KiB |

GD32H779V_START 的 board override 使用以下专用布局。NVMC 头文件同时明确给出
`MAIN_ECNVM_DECNVM1_BASE_ADDRESS=0x08900000`、`MAIN_ECNVM_DECNVM1_SIZE=0x80000`，
因此 KV 与当前从 `0x08200000` 起执行的代码位于不同 ECNVM 数据区。

| 区域 | 地址与大小 | 说明 |
| --- | --- | --- |
| `ecnvm_code` | `[0x08200000,0x08900000)`，7 MiB | `.text/.rodata` 可执行负载；SWD 烧录上限严格为 7 MiB |
| `kv` | `[0x08900000,0x08980000)`，512 KiB | Data ECNVM1；runtime 4 KiB 页擦除、8/32 B 编程 |
| `tuya_uf` | `[0x08900000,0x08940000)`，256 KiB | Tuya UF 子区 |
| `tuya_kv_data` | `[0x08940000,0x08950000)`，64 KiB | 授权及 KV 数据 |
| `tuya_kv_key` | `[0x08950000,0x08951000)`，4 KiB | KV key |
| `tuya_kv_swap` | `[0x08951000,0x08955000)`，16 KiB | KV swap |
| `tuya_user0` | `[0x08955000,0x08975000)`，128 KiB | user0 |
| `tuya_kv_protect` | `[0x08975000,0x08977000)`，8 KiB | KV protect |
| reserved test page | `[0x08977000,0x08978000)`，4 KiB | 仅用于显式 ECNVM 擦写回环；其余 32 KiB reserved |

ECNVM 命令函数放在 AXI SRAM，执行期间关闭中断；返回 ECNVM 前必须等待 BUSY
清零。写入源只接受生成布局中的 AXISRAM `[0x24000000,0x240C0000)`；ECNVM、
CNVM、未映射地址和越界指针均拒绝。KV 位于独立的 Data ECNVM1，但当前应用代码
与擦写命令仍属于同一个 ECNVM 存储块，不能在目标 KV 页取指或读取数据。
`ecnvm_kv_selftest run` 只操作 `0x08977000` 的 reserved 页：擦除、
写 32 B pattern、回读比较/CRC、再擦除并全页校验 `0xFF`；不会访问
`tuya_uf`、`tuya_kv_data` 或授权数据。

启动文件保留原厂中断向量，单独实现 GinoPi 复位入口，初始化 AXI SRAM 后复制 `.data`，调用原厂 `SystemInit()`、C 初始化及 `main()`。当前使用原厂默认 600 MHz 时钟配置、25 MHz HXTAL，板级初始化将 VTOR 设为 `0x08010000`。这些参数需要上板核对。

日志/CLI 默认使用 USART5 PC6/PC7 AF7，115200 8N1。AT transport 和 NRST 引脚由平台 Kconfig 的 `ENABLE_GD32H77X_CONSOLE_*`、`ENABLE_GD32H77X_AT_*` 选项选择；H779 当前配置为 UART4 PB13/PB12 AF14 和 PC5。`platform_config.py` 从选择派生 UART/GPIO clock、IRQn、IRQHandler 和 AT RX DMAMUX request，并拒绝 UART 或 GPIO 引脚冲突。

`gd32h77x_os/project/` 下的所有工程地位相同，包括 `tuya`、AT
桥、诊断 CLI 和复位工具。平台公共 CMake 只消费每个工程 `project.cmake`
声明的通用输入：

```text
源码
include 目录
预编译 .a
依赖 target
必要链接符号
编译定义
```

公共后端不识别 `tuya`、`tuya_app_main`、`libtuyaapp.a` 或具体工程名。

AT 单次探测、AT 串口桥和板级诊断 CLI 不再属于产品平台 Kconfig，
相关 standalone 工程位于：

```text
platform/GD32H77X/gd32h77x_os/project/board_diag/
platform/GD32H77X/gd32h77x_os/project/at_bridge/
platform/GD32H77X/gd32h77x_os/project/at_reset/
```

这些 standalone 工具不依赖 TAL/TKL，也不能放进跨平台的 TuyaOpen `examples/`。TAL/TKL 适配验证位于 `platform/GD32H77X/tuyaos/tests/adapter_diag/`。

`gd32h77x_os/project/` 下的应用统一使用平台原生构建入口，不经过 `tos.py build`：

```bash
python3 platform/GD32H77X/gd32h77x_os/tools/build_project.py \
  platform/GD32H77X/gd32h77x_os/project/at_bridge build \
  --chip GD32H779VW --board GD32H779V_START
```

`build_example.py` 是 TuyaOpen 入口钩子：编译产物来自 TuyaOpen，
它选择 `project/tuya`，并把外部 include 和 `.a` 转换成通用工程输入。
`build_project.py` 是 standalone 入口，两者最终都调用
`build_firmware.py` 和同一个 GD32H77X 固件组装后端。

## 原生诊断串口 CLI

`board_diag` 使用平台原生 console，只依赖 GD32 SDK、FreeRTOS 和 target。
Console 与日志共用当前 Console UART（H779 默认 USART5 PC6/PC7），115200 8N1。

| 命令字 | 验证内容 |
| --- | --- |
| `help` | 打印全部已注册命令，确认 CLI 已启动 |
| `diag_all` | 执行原生 FreeRTOS、堆、队列、信号量、互斥锁和线程检查 |
| `diag_info` | tick、当前空闲堆和最低空闲堆 |
| `diag_tick` | 20 ms `vTaskDelay()` 的实际跨度 |
| `diag_heap` / `diag_memory` | FreeRTOS heap 分配、读写和释放 |
| `diag_queue` / `diag_semaphore` / `diag_mutex` | FreeRTOS 同步原语语义 |
| `diag_thread` | 任务创建、优先级回读和栈水位 |
| `diag_echo <token>` | 原样回显一个参数 |
| `diag_nor_probe` | 只读识别 NOR JEDEC，并读取 offset 0 的 32 字节样本 |
| `diag_at_reset [run\|isp]` | 直接驱动 AT 模组 NRST |
| `diag_at_probe` | 复位 AT 模组并通过当前 transport 发送 `AT` 探测 |
| `diag_at_net` | 查询 AT 模组 AP IP、客户端和 server 状态 |

每条有结果的 `diag_*` 输出一行
`DIAG_RESULT <name> PASS|FAIL checks=<n> failed=<n>`。

TAL/TKL、`sys_*`、KV raw 和 TKL random 等适配层检查位于
`platform/GD32H77X/tuyaos/tests/adapter_diag/`，使用 `tos.py build`，
不会进入 `gd32h77x_os/project/` 原生构建。

当前限制：

- `diag_nor_probe` 只执行 JEDEC 识别和读取，不会编程或擦除 NOR。
- 原生诊断不执行 KV 写擦；破坏性 TKL KV 测试只在适配层诊断工程中保留。
- AT 探测要求宿主侧没有其他进程占用 Console UART；模组 transport 本身由 H77 独占。

`gd32h77x_os/tools/firmware/verify_image.py` 校验真实 ELF/BIN 的启动表、SP、复位入口、向量表、FreeRTOS 的 SVC/PendSV/SysTick、中间填充值和内存边界。它不替代硬件启动、DFU 回读或配网验收。分区、路由和烧录约束见 [Flash 分区与板级覆盖](docs/flash-layout.md)。

## 主机端协议测试

从 TuyaOpen 主仓根目录运行以下 host tests。测试会从当前 board 的 `partitions.csv` 和可选 `flash_devices.csv`（板级设备表缺失时回退芯片默认，分区表缺失时使用平台内部 fallback）调用 resolver 并生成布局头文件和 C 表；不需要先构建专用 bring-up example：

```bash
cmake -S platform/GD32H77X/tests -B /tmp/gd32h77x-host-tests -G Ninja \
  -DPython3_EXECUTABLE="$OPEN_SDK_PYTHON"
cmake --build /tmp/gd32h77x-host-tests
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/gd32h77x-host-tests --output-on-failure
python3 platform/GD32H77X/tests/test_flash_bridge.py
```

默认启用 ASan 和 UBSan。受 ptrace 限制的运行环境无法使用 LeakSanitizer 时，设置 `ASAN_OPTIONS=detect_leaks=0` 可继续执行地址和未定义行为检查。解析器不进行动态内存分配。

主机测试覆盖 AT parser/client 的二进制拆包、命令应答、发送提示符、超时恢复及协议错误；Flash 跨 segment 路由、实际 resolver 生成布局的边界与 unsupported 语义、混合擦除几何及零长度/溢出；桥接 stale/tampered artifact 拒绝和 DFU 设备选择；熵 helper 的缓存、重复样本、健康错误、超时清零及并发占用语义。熵测试通过 mock reader 执行，不证明硬件随机源质量或时钟配置。

Flash 与熵 helper 的边界、限制和复现命令见 [存储与熵状态](docs/storage-entropy-status.md)。

从主仓根目录可以对构建出的真实镜像执行破坏性输入检查，确认启动表、代码、向量表、截断及额外尾数据被校验器拒绝：

```bash
python platform/GD32H77X/tests/test_image.py \
  examples/system/gd32h77x_bringup/.build/gd32h77x/firmware.elf \
  examples/system/gd32h77x_bringup/.build/gd32h77x/firmware.bin \
  examples/system/gd32h77x_bringup/.build/gd32h77x/generated/memory/resolved_layout.json
```

配网所需的 553 能力与缺口见 [无线适配清单](docs/vw553-capabilities.md)。

## VW553 无线协处理器

当前 VW553 AT gateway 的唯一 canonical 构建入口位于：

```text
platform/GD32/gd32_os/MSDK/vw553_at_gateway
```

从 `platform/GD32/gd32_os` 执行：

```bash
./cmake_build.sh vw553_at_gateway clean
./cmake_build.sh vw553_at_gateway
```

烧录镜像：

```text
platform/GD32/gd32_os/scripts/images/image-all.bin
```

应用说明见 [VW553 AT gateway](../GD32/gd32_os/MSDK/vw553_at_gateway/docs/README.md)，
ISP 烧录见 [VW553 UART ISP](docs/vw553-isp-flashing.md)。

当前稳定 UART 镜像同时预置在 H77 仓库：

```text
platform/GD32H77X/wireless_fw/vw553/uart/vw553_at_gateway_uart_v0.5.2.bin
```

其版本、引脚、SHA256 和烧录命令见
[wireless_fw/vw553](wireless_fw/vw553/README.md)。SPI gateway 尚待开发和实板验收。

不要再使用旧的 `/tmp/gd32vw553-diag`、`MSDK/projects/vw553_at_gateway`、
外部 `cmake_build` 工程目录、overlay/staging 或手工 `patch -p*` 作为当前构建步骤。
这些内容只允许作为历史证据保留，并应明确标注为历史记录。
