# GD32H77X components

`gd32h77x_os/components/` 是平台原生组件根目录，本身不参与 CMake 构建，也不在这里放 `CMakeLists.txt`。根平台 `platform/GD32H77X/CMakeLists.txt` 会自动遍历 `components/*/CMakeLists.txt`，发现一个组件就 `add_subdirectory()` 一个组件。

当前组件：

```text
components/
├── FreeRTOS/   FreeRTOS kernel 和 Cortex-M7 port
├── at/         AT client/parser/session/socket 与 transport
├── console/    调试控制台
├── drivers/    CNVM/ECNVM/NOR/Flash route/entropy
├── runtime/    启动后的平台运行时、syscall 和共享 hook
└── startup/    reset entry、main 和 linker template
```

依赖方向：

```text
project/<app>
    -> components/*
    -> gd32/gd32h77x_gd32_sdk

TuyaOpen TKL/TAL
    -> tuyaos_adapter
    -> components/at
```

约束：

- 组件目录自行维护 `CMakeLists.txt`。
- 组件不得依赖 `project/<app>`、TuyaOpen `src/`、TKL/TAL 或 `OPRT_*`。
- `gd32/` 独立维护 SDK、CMSIS、标准外设和 `gd32h77x_gd32_sdk`。
- 新增组件只需创建子目录和 `CMakeLists.txt`，不修改 components 根构建文件。
