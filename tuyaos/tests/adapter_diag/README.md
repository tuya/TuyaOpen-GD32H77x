# TuyaOS adapter diagnostics

This is a TuyaOpen application for validating the GD32H77X TAL/TKL adapter.
It intentionally uses `tal_*`, `tkl_*` and `OPRT_*` APIs and therefore does not
belong in the standalone `gd32h77x_os/project/` layer.

Build it with the normal TuyaOpen application build:

```bash
cd platform/GD32H77X/tuyaos/tests/adapter_diag
tos.py config set \
  CONFIG_BOARD_CHOICE_GD32H77X=y \
  CONFIG_BOARD_CHOICE_GD32H779V_START=y \
  CONFIG_ENABLE_UART=y \
  CONFIG_ENABLE_SERIAL_CLI_CMD=y \
  CONFIG_SERIAL_CLI_STACK_SIZE=8192
tos.py build
```

`app_default.config` must not be stored in this directory. The configuration
above is a local build setup and must not become a checked-in application
configuration file.
