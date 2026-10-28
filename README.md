# TuyaOpen GD32H77x 平台

平台子仓库：`git@github.com:tuya/TuyaOpen-GD32H77x.git`。首板为 GD32H77D GinoPi，后续扩展 H779。底层使用指定原厂 SDK，运行 FreeRTOS 11.2.0，不引入 RT-Thread SDK、设备框架或 SCons。

## 当前状态

截至 2026 年 10 月 20 日，当前实现覆盖主仓构建集成、原厂启动与驱动依赖、FreeRTOS、日志、TKL 线程/内存/互斥锁/信号量/队列/时间、生成布局表驱动的 Flash 地址路由、硬件熵采集 helper、UART AT 接收解析器和独立事务状态机。布局 manifest 绑定 board/chip、布局摘要、ELF/BIN 名称和 digest；构建会生成含 ELF、BIN、报告及显式外部 payload 的自包含 `.tufw` 包，flash bridge 烧录前重新解析当前布局并复核镜像。CNVM 有受限 DFU/SWD/UART ROM ISP 写入路径；GinoPi 外部 NOR 有基于 SRAM loader 的 SWD 写入路径。2026 年 10 月 20 日已在 GinoPi 实板确认 H77x ROM UART ISP：`/dev/ttyUSB1`、115200 8E1、GETID `44495764FFFFFFFF`，并通过 `tos.py flash` 对 CNVM 完成擦除、写入和逐字节回读。外部 OSPI 的配置字节与 pin record 已按厂商 CLI 默认路径准备，但仍待实板写入回读验收；不能据此认为 UART 外片路径已可用。布局绑定驱动的 GD25Q64E OSPI 运行时协议层现含 JEDEC、读取、页编程和 4 KiB 擦除，以及主机模型测试；这些 NOR 写擦 API 尚未接入 TKL Flash/backend，也尚未在目标板验证，调用不得视为硬件验收。CNVM KV 读写擦代码和主机测试已完成，但目标板 KV 写擦及缓存行为尚未验证；OTA 流程仍未实现。`tos.py build` 生成 ELF、MAP、完整烧录 BIN 和 `.tufw` 包，并执行静态布局校验。历史设备运行和 DFU 状态见下方既有记录。

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

`gd32h77x_os/gd32/` 与 `gd32h77x_os/FreeRTOS/` 中的 178 个文件直接从该归档提取，未修改原始内容。`gd32h77x_os/manifest.json` 记录这些原厂文件的归档摘要、逐文件原始路径及 SHA256，`platform_prepare.py` 在构建前验证。平台自维护的 `gd32h77x_os/chips/` 芯片配置与 linker 模板不在原厂 manifest 内。保留原厂许可及 FreeRTOS/CMSIS 许可证；本仓自行实现的代码采用 Apache License 2.0。

重新导入需要 Python 3.11 或以上及 libarchive：

```bash
python gd32h77x_os/tools/firmware/import_sdk.py /path/to/GD32H77x_78x_Demo_Suites_V0.5.0.7z
```

导入器只接受上述摘要。请使用原始压缩包，不从其他 BSP 或临时 SDK 工作副本混入依赖。

## 在 TuyaOpen 主仓构建

将本仓置于主仓 `platform/GD32H77X/`，主仓还需要相应的 `boards/GD32H77X/`、平台清单和 `examples/system/gd32h77x_bringup/` 集成。

```bash
# 在 TuyaOpen 主仓根目录执行
. ./export.sh
export GD32H77X_TOOLCHAIN=/opt/gcc-arm-none-eabi-10.3-2021.10
tos.py check
cd examples/system/gd32h77x_bringup
tos.py config set BOARD_CHOICE_GD32H77X=y BOARD_CHOICE_GD32H77D_GINO=y
tos.py build
```

已通过构建验证的工具链是 GNU Arm Embedded 10.3-2021.10，编译器版本 10.3.1；准备脚本检查该版本。未设置 `GD32H77X_TOOLCHAIN` 且 `PATH` 中没有 `arm-none-eabi-gcc` 时，`platform_prepare.py` 和 `toolchain_file.cmake` 会自动探测 `/opt/gcc-arm-none-eabi-*`。CPU/ABI 参数统一为 `-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=softfp`。原厂工程记录的 GCC 15.2.1 尚未验证。

FreeRTOS 使用原厂归档内的 `GCC/ARM_CM7/r0p1` port。该 port 包含早期内核修订的勘误处理，FreeRTOS 随包说明允许用于全部 M7 修订；首次运行日志打印 CPUID，供实板核对。任务优先级 0 保留给 idle，TKL 优先级映射为 FreeRTOS 优先级加 1；栈及水位值按 TKL 要求使用字节。有限等待向上换算 tick，`UINT32_MAX` 表示永久等待。

构建中间目录为应用的 `.build/gd32h77x/`，不同应用不会共用平台构建目录。最终文件位于 `dist/gd32h77x_bringup_1.0.0/`：

| 文件 | 用途 |
| --- | --- |
| `gd32h77x_bringup_QIO_1.0.0.bin` | 从 `0x08000000` 写入的完整镜像；QIO 是主仓完整镜像的命名约定，不表示 H77 使用 QSPI 启动 |
| `gd32h77x_bringup_1.0.0.elf` | 符号及调试信息 |
| `gd32h77x_bringup_1.0.0.map` | 链接布局及资源占用 |
| `gd32h77x_bringup_1.0.0.layout.json` | 所选 layout 来源/摘要、ELF/BIN digest、烧录范围及静态布局检查结果 |
| `gd32h77x_bringup_QIO_1.0.0.tufw` | 自包含烧录包，含 build-bound report、ELF、CNVM BIN，以及按构建配置选择的外部 payload |

### 通过 `tos.py flash` 烧录 H77D GinoPi

主仓 `tos.py flash` 通过可选平台 artifact hook 选择 `.build/bin/<project>_QIO_<version>.tufw`；若当前构建没有包则沿用 BIN。包自带报告、ELF、CNVM BIN 和 payload，不依赖旁边的 ELF/BIN/report 文件。包验证器用 `boards_root` 重新解析当前布局，并比较 source/layout digest、板型、芯片、分区路由和镜像摘要；缺失或过期内容直接拒绝。旧的独立 BIN 烧录仍要求同目录 `<project>_<version>.layout.json`、ELF 与 BIN 完整且摘要一致。CNVM 写入范围始终限于 boot+primary，H77x 扇区为 64 KiB，临时镜像按扇区补 `0xFF`；不会把 OTA staging、metadata 或 KV 自动拼入 BIN。

如需在一次构建中显式选择片外分区 payload，在应用根目录创建 `flash_payloads.csv`，列头必须是 `partition,file`，每行选择一个分区，文件路径相对 CSV 所在目录。例如：

```csv
partition,file
filesystem,images/filesystem.bin
```

构建默认在 CSV 存在时将其选中的 payload 放进 `.tufw`。若需要保留 CSV 但生成经过相同 layout、report、ELF 和 BIN 校验的纯片内包，可显式关闭构建选项：

```bash
tos.py config set CONFIG_GD32H77X_PACKAGE_INTERNAL_ONLY=y
tos.py build
```

该选项只控制 payload 是否进入本次 package，不改变 layout 或 CSV；默认选择包含 CSV payload，保持原构建行为。恢复默认行为可执行 `tos.py config set -u CONFIG_GD32H77X_PACKAGE_INTERNAL_ONLY`。保护分区不会自动加入包，boot/primary 不能通过 CSV 覆盖。GinoPi 外 NOR 的 SWD 写入流程已实现：先复核 layout、pack、loader、同一 probe 与 NOR 只读身份/保护状态，再逐分区擦写并回读；这条外片路径尚未实板验收。通用 `uart` backend 可写入 manifest 与逐设备 profile 均通过校验的外部设备；当前 GinoPi 尚无相应 UART profile，因此不能据此宣称它的 UART 外 NOR 已可用。外片写入完成后才写 CNVM boot+primary；中间失败会停止后续阶段，不承诺跨器件回滚。DFU 后端在枚举设备或擦写 CNVM 前拒绝包含外部 payload 的包。`CONFIG_FLASH_VERIFY=n` 时会明确报告回读已跳过。

SWD 外片写入当前只支持 GD25Q64E（布局器件型号 `GD25Q64ESIG`），以 GPIO 模拟单线 SPI，不启用 Quad/QE，也不依赖运行时 TKL NOR 驱动。loader 的 MCU canonical target 与 GPIO 接线均来自 resolved manifest；当前写入流程一次只处理一个受支持 NOR 设备。主机还须有 ARM GCC 与配套 `arm-none-eabi-objcopy`，bridge 会即时编译 SRAM loader；仅安装 pyOCD 不足以使用此后端。单独构建时必须显式提供生成的 layout manifest，由它选取 target 和该 NOR 的 `board_binding`：

```bash
ARM_NONE_EABI_GCC="${ARM_NONE_EABI_GCC:-${GD32H77X_TOOLCHAIN:+$GD32H77X_TOOLCHAIN/bin/}arm-none-eabi-gcc}"
LOADER_OUT="$(mktemp -d /tmp/gd32h77x-ospi-loader.XXXXXX)"
LAYOUT=examples/system/gd32h77x_bringup/.build/bin/gd32h77x_bringup_1.0.0.layout.json
"$OPEN_SDK_PYTHON" platform/GD32H77X/gd32h77x_os/tools/download/ospi_loader/build_loader.py \
  --compiler "$ARM_NONE_EABI_GCC" --out-dir "$LOADER_OUT" --layout "$LAYOUT"
```

`GD32H77X_TOOLCHAIN` 指向 GNU Arm Embedded 安装目录时，上述命令会从其 `bin/` 目录选 GCC；也可直接设置 `ARM_NONE_EABI_GCC`，或让 `arm-none-eabi-gcc` 与 `arm-none-eabi-objcopy` 位于 `PATH`。烧录时使用同一环境变量供即时构建使用。

`flash.cfg` 的 `CONFIG_FLASH_BACKEND` 选择烧录实现，各支持的后端消费同一份布局与几何：

| 后端 | 说明 |
| --- | --- |
| `dfu` | 原生 DfuSe，经 ctypes 调用 libusb-1.0，不依赖外部 `dfu-util`，也不采用 ROM 报告的 DfuSe 描述符几何 |
| `swd` | 原厂 CMSIS-Pack Flash 算法，经固定版本 pyOCD 编程；校验官方 pack/target/算法几何，sector erase 后回读含 `0xFF` padding 的完整写入扇区 |
| `uart`（可选） | H77x ROM system-memory bootloader，经串口捕获 ROM 响应并保持该波特率；先写显式 OSPI payload，再写 CNVM，不依赖 SWD、pyOCD 或 CMSIS-Pack。GinoPi 默认使用已实测的 `dfu` |

原版 `dfu-util` 后端已禁用：ROM descriptor 报告 4 KiB unit，而 H77 CNVM registry 使用 64 KiB sector；原工具在该 ROM 上的擦除行为未作为本平台可信路径。使用原生 `dfu`、`swd` 或 `uart`。原生 DFU 只接受 DFU-mode protocol 2 alternate、DfuSe 1.1a、支持 download 且默认回读时支持 upload 的 alternate，并检查 alt 名称仍标识从 `0x08000000` 开始的 internal flash。此信息能确认内部 flash bank，不能自动探测具体料号或容量；bridge 以 build manifest 的 canonical chip 与板级 `CONFIG_FLASH_TARGET_CHIP` 交叉核对，VID/PID、USB serial、端口路径和 alt 均只是可选筛选条件。

### GD32H779V START 的诊断代码 ECNVM 分段

`CONFIG_GD32H77X_ECNVM_CODE` 默认关闭，仅在 `GD32H779V_START` 且启用 `GD32H77X_DIAG_CLI` 时可选。板级布局始终声明 `0x08200000` 起的首个 64 KiB `ecnvm_code` 区；打开选项后，诊断 CLI 的 `.text/.rodata` 链接到该区域，`.tufw` 会携带独立的 ECNVM payload 和布局/摘要绑定信息。此配置必须使用完整 `.tufw`，bridge 会拒绝 raw BIN 和非 SWD 后端；SWD 先按 ECNVM 扇区擦写并强制读回，再写 CNVM。`CONFIG_FLASH_VERIFY=n` 不会关闭 ECNVM 读回。ECNVM runtime 擦写及 OTA 仍未实现；本次正式双区包尚待板级回归验证，不能据此视为硬件烧录验收。

平台的器件身份来自 `gd32h77x_os/chips/platform_chips.json`，共支持 D/E/779/RAP 的 16 个 canonical DFP target。`GD32H77D` 是兼容 alias，仅允许已有 GinoPi board 使用，映射到 `GD32H77DIW`；其他 variant 必须在 build identity 中选具体料号。Flash registry 的容量、64 KiB sector 和算法名必须与本地原厂 pack 一致。2 MiB 使用 `GD32H77x_78x_CNVM_2M.FLM`，1.625 MiB 使用 `GD32H77x_78x_CNVM_1M_640K.FLM`。SWD target 不能覆盖 layout/manifest 的 canonical target。

SWD 可选依赖固定在 `requirements-swd.txt`，通过 SDK Python 安装：

```bash
UV_CACHE_DIR=/tmp/tuyaopen-uv-cache ./.tools/uv/0.11.18/uv pip install \
  --python "$OPEN_SDK_PYTHON" -r platform/GD32H77X/requirements-swd.txt
```

`export.sh` 后续执行 `uv sync` 可能移除额外安装的可选包；若 pyOCD 不存在或版本不符，重新执行此安装命令。烧录步骤不会自动下载或安装依赖。

从 [GigaDevice 官方资料下载页](https://www.gd32mcu.com/en/download?kw=GD32H77D) 获取 GD32H77D-Gino SDK/AddOn 内附的 `GigaDevice.GD32H77x_78x_DFP.0.6.1.pack`；该版本也随 [GinoPi SDK 源码包](https://github.com/RT-Thread-Studio/sdk-bsp-gd32h77d-realthread-ginopi) 存放于 `tools/mdk_pack/`。官方资料站也可能列出面向其他 H7 系列的新 pack；烧录工具固定校验上述 PDSC 身份/版本与 SHA256，不要替换成不同版本或摘要的 pack。将文件放到本机路径并设置 `GD32H77X_DFP_PACK` 或 `CONFIG_FLASH_SWD_PACK`。获准 pack SHA256 为 `94ed659f9adb58c881114858e9a13dba4a0540765c4d9e4d305a24e13ca5dee6`；bridge 固定要求 pyOCD 0.46.0 与此 pack。仓库开发环境当前已用该 pack 和 pyOCD 对 16 个 target 做过无硬件算法/几何校验；尚未据此宣称 16 个型号或 Gino SWD 已实板验收。

让 GinoPi 进入 USB DFU 模式后在应用目录执行：

```bash
tos.py flash
# 同时有多个匹配 DFU 设备时，用 USB 总线端口路径精确选择：
tos.py flash -p 3-3.2
# SWD 时 -p 传 pyOCD probe unique ID（可使用完整或唯一子串）
tos.py flash -p cmsis-dap:12345678
```

bridge 在 USB/SWD 操作前校验 layout report、ELF/BIN digest、当前 CSV 来源及 allowwrite 范围。DFU 按 VID/PID 和可选 serial/path 筛选，再确认 DFU mode、协议版本与内部 flash alternate；多设备时 TTY 可选，非 TTY 必须用 `-p` 指定 USB 路径。SWD 的 `-p` 是 pyOCD probe ID，非 TTY 多 probe 同样拒绝歧义；TTY 才允许明确选择。SWD 使用隔离临时目录、`--no-config` 与空 user script，强制 sector erase、`auto_unlock=false`、`keep_unwritten=false`、`smart_flash=false`、断开不自动运行，且成功写入后保持复位/停止状态。默认 readback 对完整 FF 填充后的扇区做逐字节比较；`CONFIG_FLASH_VERIFY=n` 可关闭回读。原生 `dfu` 默认同样回读 padded bytes；成功后仍按板级流程手动复位/退出 ROM。

#### UART ROM ISP（不依赖 SWD/pyOCD）

`flash.cfg` 设 `CONFIG_FLASH_BACKEND=uart` 后，bridge 改用 H77x ROM system-memory bootloader：主机使用串口和 BOOT 复位时序，不枚举 USB、不启动 pyOCD、也不读取 CMSIS-Pack。自动波特率捕获 ROM 响应后保持该波特率；不承诺无需复位即可切换到更高速率。缺少 `CONFIG_FLASH_UART_EXPECTED_ID` 时会在串口枚举前拒绝运行，值必须是经目标板单独核实的完整原始 GETID 响应，并由 `CONFIG_FLASH_TARGET_CHIP` 绑定到 canonical target。该路径可用于不依赖 SWD 的烧录，但身份绑定本身不代表所有芯片变体都已验证。擦除、写入和回读每 64 KiB（擦除每 1 MiB）输出进度、吞吐与 ETA。

```bash
# 在应用目录内，先按上节完成构建并生成 .tufw
tos.py flash -p /dev/ttyUSB0
```

可用只读探针查看候选串口上的握手和 `GETID`（不会擦除或写入）：

```bash
./.venv/bin/python platform/GD32H77X/gd32h77x_os/tools/diagnostics/gd32h77x_uart_probe.py \
  -p /dev/ttyUSB0 --timeout 30
```

- 未指定 `-p` 或 `CONFIG_FLASH_UART_PORT` 时，工具枚举串口：单个候选自动选择；多个候选在 TTY 中列出编号供选择，非 TTY 模式要求显式指定 `-p`。指定端口时不会枚举其他串口。
- 写序为「先 OSPI 外片 payload，后 CNVM boot+primary」。CNVM 阶段失败时外片可能已经写入，不承诺跨器件回滚；`dfu` 后端仍然拒绝带外部 payload 的包。
- 外片 payload 来自构建时的 `flash_payloads.csv` 并随 `.tufw` 分发；没有外部 payload 的包只写 CNVM。方案不再带有 SWD SRAM loader 与 probe 复核，只按 ROM 的 OSPI 配置、4 KiB 扇区擦除与回读执行。
- 可覆盖项：`CONFIG_FLASH_UART_PORT`（等价 `-p`）、`CONFIG_FLASH_UART_BAUD`（`auto` 或具体整数，未设置即 `auto`）、`CONFIG_FLASH_UART_VERIFY`（串口专用回读开关）、`CONFIG_FLASH_UART_OSPI_PROFILES`（相对 `flash.cfg` 的 CSV 路径，列为 `device,config,pins`；每个 device 对应一条配置和由 manifest binding 决定数量的分号分隔 pin records）、`CONFIG_FLASH_UART_ERASE_MODE`（`extended`/`base`）。包含外部 OSPI payload 时必须提供已核实的逐设备 profile；不会使用隐式 ROM 默认值。`-b` 接受具体整数，只对 `uart` 后端有效，其他后端传入会被拒绝。
- 默认回读逐字节比较 OSPI 分区和 CNVM 镜像。`CONFIG_FLASH_UART_VERIFY=n` 只关闭串口回读，不影响 DFU/SWD；未设置该项时才回退到 `CONFIG_FLASH_VERIFY`。写入时间随 ROM 捕获或显式设置的波特率变化；关闭回读可以缩短耗时，但会跳过写入结果比对。

2026 年 10 月 20 日曾记录 GinoPi UART CNVM 烧录与回读通过；后续复验未能重复完成 ROM READ。921600 8E1 的握手和 GETID 成功，164348 字节镜像写入完成，但读回校验超时；115200 只读复验取得 32 KiB 后也超时；最近一次 9600 尝试在握手阶段无响应，未进入 READ。这些结果不能证明物理波特率不可靠，仍需排查 ROM READ 协议、主机接收处理和超时设置。UART CNVM 验收暂缓，直到完整 boot+primary 写入与补齐扇区读回均通过。UART 外部 NOR 也未验收：还需完成 SRAM loader 的写入、擦除、读回和保护范围检查。不得据此宣称 UART NOR 已通过实板验收。

### 烧录工具离线复验

2026 年 10 月 20 日的无硬件回归共 82 项：layout 21、DFU transport 19、bridge 26、SWD backend 9、官方 pack/pyOCD targets 5、variant integration 2（其中遍历全部 16 个 canonical target）。Pack 测试使用本机官方 DFP；以下路径请替换为你的 0.6.1 pack 路径。DFU 使用内存设备模拟，SWD probe/program/readback 均 mock；pyOCD target 列表查询不枚举 probe。本轮没有 USB 访问或硬件写入。

```bash
PY="$OPEN_SDK_PYTHON"
"$PY" platform/GD32H77X/tests/test_memory_layout.py
"$PY" platform/GD32H77X/tests/test_dfuse.py
"$PY" platform/GD32H77X/tests/test_flash_bridge.py
"$PY" platform/GD32H77X/tests/test_swd_backend.py
GD32H77X_DFP_PACK=/path/to/GigaDevice.GD32H77x_78x_DFP.0.6.1.pack \
  "$PY" platform/GD32H77X/tests/test_swd_pack.py
"$PY" platform/GD32H77X/tests/test_flash_variants.py
```

2026 年 10 月 20 日的烧录包增量复验另通过 104 项：CLI artifact hook 23、flash bridge 42、UART ROM ISP 20、package 11、OSPI writer 8（此前记录为 78 项，本轮补入 UART ISP 主机测试并重跑 bridge）。UART ROM ISP 测试用协议模型覆盖握手、GETID、自动波特率回退、CNVM 与 OSPI 擦写、地址映射、回读失败和 OSPI 失败不触碰 CNVM；不连接串口。bridge 测试包含由 `tos.py build` 自动生成的真实 Gino `.tufw`：将包单独放入空目录，使用真实 package validator/current resolver，再由 DfuSe 内存模型完成写入与读回；OSPI 计划测试验证真实打包 payload、分区边界、扇区计划并实际编译 SRAM loader，还用 8 MiB NOR 协议模型覆盖实际擦写命令循环、读回、超时及失败阶段。只 mock SWD target边界；没有连接 USB/probe，也没有写入 CNVM 或 NOR。旧的 82 项汇总是此前代码阶段的记录，与本段增量数字分开统计。

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

启动文件保留原厂中断向量，单独实现 GinoPi 复位入口，初始化 AXI SRAM 后复制 `.data`，调用原厂 `SystemInit()`、C 初始化及 `main()`。当前使用原厂默认 600 MHz 时钟配置、25 MHz HXTAL，板级初始化将 VTOR 设为 `0x08010000`。这些参数需要上板核对。

日志使用 USART1 PA2/PA3，115200 8N1。主机通信使用 UART4 PB12/PB13 AF14，553 NRST 为 PC5；当前 bring-up 镜像不复位或重配 553。

`GD32H77X_AT_PROBE` 默认关闭。需要测试 UART4 时执行 `tos.py config set GD32H77X_AT_PROBE=y` 后重新构建；固件启动会发送一次 `AT`，最多等待 3 秒，输出 `553 AT PROBE: OK` 或 `FAILED`，随后继续 OS 检查。接收使用 FreeRTOS stream buffer，由 UART4 中断写入、独立任务解析，UART 错误或溢出会使探测失败。该探测不执行联网、复位或固件更新。执行 `tos.py config set -u GD32H77X_AT_PROBE` 恢复默认。

## 诊断串口 CLI（tal_cli）

`CONFIG_GD32H77X_DIAG_CLI` 打开后，平台在 `main.c` 的 `app_entry()` 中先注册 `diag_*` 命令，再调用 `tal_cli_init()` 启动 TuyaOpen 串口 CLI（同时注册 `sys_*` 与 `help`/`hello`/`version`）。该选项只用于板级验证，正常应用构建保持关闭。

CLI 与日志共用 USART1 PA2/PA3，115200 8N1，两者输出会交错，`tuya> ` 之后即可输入命令字。启用方式：

```bash
tos.py config set CONFIG_ENABLE_UART=y CONFIG_ENABLE_SERIAL_CLI_CMD=y CONFIG_GD32H77X_DIAG_CLI=y
tos.py config set CONFIG_CLI_CMD_FS=n CONFIG_CLI_CMD_KV=n   # KV/Flash 尚未开放写入
```

`examples/system/gd32h77x_bringup/app_default.config` 已包含上述设置。`CONFIG_ENABLE_UART` 需要在 `boards/GD32H77X/TKL_Kconfig` 中显式打开；该板默认 `n`，因为它同时打开日志与 CLI 使用的 UART 适配。

| 命令字 | 验证内容 |
| --- | --- |
| `help` | 打印全部已注册命令，确认 CLI 已启动 |
| `diag_all` | 依次执行下表 8 项自检，输出单行汇总 |
| `diag_info` | `tal_system_get_millisecond()` 与空闲堆快照 |
| `diag_tick` | 20 ms `tal_system_sleep()` 的实际跨度（10–1000 ms 之间为通过） |
| `diag_heap` | 空闲堆读取 |
| `diag_memory` | `tal_calloc`/`tal_realloc` 清零、保留数据、拒绝 `SIZE_MAX` 溢出 |
| `diag_queue` | 队列 FIFO、满队列拒绝、空队列超时 |
| `diag_semaphore` | 信号量 post/wait 与超时 |
| `diag_mutex` | 递归互斥锁重复加锁/解锁 |
| `diag_thread` | 工作线程创建、优先级回读、栈水位（字节） |
| `diag_echo <单参数>` | 原样回显一个含 `%` 的参数，验证回显不当作格式串 |
| `diag_kv_raw_test ERASE_KV_SECTOR0` | 显式破坏性 TKL 原始路径诊断：仅擦除生成布局中 KV 的首个 64 KiB 扇区，验证擦除态、写入并读回固定 32 字节 |
| `diag_nor_probe` | 仅初始化受支持 NOR、报告 JEDEC，并只读 offset 0 的固定 32 字节和 FNV-1a 32 位摘要 |
| `sys_heap` / `sys_thread` / `sys_version` / `sys_status` / `sys_tick` | 框架 `sys_*` 命令组在本平台的可用性 |
| `sys_random [count [range]]` | 逐次调用 `tal_system_get_random()` → `tkl_system_get_random()`，默认 16 次、范围 256；`range=0` 使用 TKL 兼容的 8-bit 范围，检查返回值边界并打印十进制/十六进制样本 |

每条 `diag_*` 输出一行 `DIAG_RESULT <name> PASS|FAIL checks=<n> failed=<n>`；带参数的命令若参数个数不符会直接判为 `FAIL`。`diag_all` 汇总全部 8 项，收到 `DIAG_RESULT diag_all PASS checks=<n> failed=0` 即表示该轮 TAL/TKL 自检全部通过。

`sys_random` 只验证接口可调用次数与范围契约，不把重复值判为失败，也不宣称随机质量通过。H77 当前在 TRNG 失败时 fail-closed 并输出带错误阶段/寄存器快照的 `PANIC`；若命令在首个样本处停止，应保存该行完整串口日志用于继续定位。

当前限制：

- `fs_*`、`kv_*` 命令组默认关闭：LittleFS 尚未挂载，且目标板 KV 写擦/缓存行为尚未验收；启用这些命令前需完成硬件验证，默认关闭状态保持不变。
- `diag_kv_raw_test` 仅在串口 CLI 中被人工显式调用时运行，不会自动执行，也不使用 `tal_kv` 或文件系统。它会擦除生成布局中 KV 的第一个擦除单元并写入测试数据；仅可在已完成且校验过完整 ROM KV 备份后使用，完成后必须从 ROM 备份恢复完整 KV 分区。此命令提供受控验证路径，不代表目标板 KV 写擦/缓存已验收。
- `diag_nor_probe` 只执行 JEDEC 识别和 32 字节读取，不会编程或擦除 NOR；FNV-1a 值用于重复读比较，不是密码学完整性校验。OSPI runtime NOR 写擦协议目前只有 host mock 覆盖，未上板验证，也未接入 TKL Flash/backend。
- `sys_iot_*`、`sys_netmgr wifi ...` 需要云连接和 Wi-Fi TKL，本平台尚未实现，只应作为“未支持”观察，不作为验收项。
- 2026-10-19 已将带 CLI 的镜像写入 H77D GinoPi 前 3 个 64 KiB 扇区（`0x08000000–0x0802FFFF`），并完成 2 MiB 全片回读：写入区与补齐镜像逐字节一致，其余扇区与写前备份一致。随后通过 UART1 完成实板命令验收：`diag_all` 汇总 31 项检查通过；8 条独立诊断命令合计 31 项检查通过，另有 `diag_echo %` 与 `sys_heap`、`sys_thread`、`sys_version`、`sys_status`、`sys_tick` 输出记录。9 条诊断结果均为 PASS，失败数为 0。原始串口记录与解析结果保存在 `.cache/gino-validation/cli-acceptance-20261019/`。

`gd32h77x_os/tools/firmware/verify_image.py` 校验真实 ELF/BIN 的启动表、SP、复位入口、向量表、FreeRTOS 的 SVC/PendSV/SysTick、中间填充值和内存边界。它不替代硬件启动、DFU 回读或配网验收。烧录与恢复资料在主仓 `docs/gd32h77x_tuyaopen_handoff.md`。

## 主机端协议测试

从 TuyaOpen 主仓根目录运行以下 host tests。测试会从当前 board 的 `partitions.csv` / `flash_devices.csv`（没有 board 分区表时使用平台内部 fallback）调用 resolver 并生成布局头文件和 C 表；不需要先构建专用 bring-up example：

```bash
cmake -S platform/GD32H77X/tests -B /tmp/gd32h77x-host-tests -G Ninja
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
