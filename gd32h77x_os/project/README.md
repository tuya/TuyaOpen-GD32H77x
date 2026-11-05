# GD32H77X project applications

`project/` 下的目录都是平级工程。平台公共构建器只处理工程声明的通用输入，
不识别具体工程名，也不判断工程是否属于 Tuya。

```text
project/
├── tuya/                 # 外部应用库 + 本地 app_entry wrapper
├── at_bridge/
├── board_diag/
└── at_reset/
```

- 每个工程只用 `project.cmake` 声明构建输入，并自己实现 `app_entry()`；工程不绑定芯片或板卡。
- `tuya/` 的应用主体来自外部 `libtuyaapp.a`，本地 `app_entry()` 只负责调用 `tuya_app_main()`。
- `at_*` 和 `board_diag` 的应用主体使用本地源码。
- 工程通过 `project.cmake` 声明源码、include、预编译库、依赖 target 和必要符号。
- 平台公共 CMake 只处理这些通用输入，不包含 Tuya/TKL/TAL 特判。
- 应用可以 include/调用 `gd32h77x_os/components/` 中的平台组件 API。
- `target/` 是固定平台层；`project/at_*` 和 `project/board_diag` 是可选原生项目。
- 不同 `project/<app>/` 之间不得相互引用或包含。
- 跨平台 TuyaOpen 应用放在主仓 `apps/` 或 `examples/`，不放这里。

standalone 工程使用同一个入口：

```bash
python3 platform/GD32H77X/gd32h77x_os/tools/build_project.py \
  platform/GD32H77X/gd32h77x_os/project/<app> build \
  --chip GD32H779VW --board GD32H779V_START
```

工程名默认取目录名，版本默认 `1.0.0`，可分别用 `--name`、`--version` 覆盖。
芯片和板卡属于本次编译参数，不写进跨平台工程源码。

`build_example.py` 是 TuyaOpen 入口钩子，负责选择 `project/tuya`，并把
TuyaOpen 的头文件和 `.a` 转成通用外部输入。`build_example.py`、
`build_project.py` 最终都调用 `build_firmware.py`，再进入
`platform/GD32H77X/CMakeLists.txt` 的公共固件组装后端。

需要验证 TAL/TKL 的应用放在 `platform/GD32H77X/tuyaos/tests/`，使用
`tos.py build` 和标准的 `tuya_app_main()`；这些内容不得回流到
`gd32h77x_os/project/`。
