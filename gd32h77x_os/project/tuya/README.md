# Tuya external-application project

该工程与 `at_bridge` 等工程平级，区别只是应用主体来自外部预编译库。

- `target/main.c` 只负责平台初始化并创建调用 `app_entry(void *)` 的 FreeRTOS 任务。
- `project.cmake` 声明本地 `app_entry.c`、外部 include、外部 `.a`、依赖 target 和必要符号。
- 该实现强引用应用库统一提供的 `tuya_app_main()`，没有 weak fallback；缺失时最终链接必须失败。
- `tuya_app_main` 来自 TuyaOpen 生成的 `libtuyaapp.a`，不是本目录实现。
- 平台公共 CMake 不需要知道 Tuya；它只执行工程声明的通用输入。
