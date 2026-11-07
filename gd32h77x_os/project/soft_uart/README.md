# GPIO software UART demo

- TX: `PF4`
- RX: `PF5`
- Format: `9600 8N1`
- TX is driven by a DWT-cycle-counter bit-banged UART.
- PF5 is configured as a pulled-up input and supports polling receive/echo.
- The app sends `SOFT_UART PF4=TX PF5=RX 9600 8N1\r\n` back-to-back, with a
  1 ms polling window between messages for RX echo.

Build with the standalone project builder:

```bash
PATH=/opt/gcc-arm-none-eabi-10.3-2021.10/bin:$PATH \
./.venv/bin/python platform/GD32H77X/gd32h77x_os/tools/build_project.py \
  platform/GD32H77X/gd32h77x_os/project/soft_uart build \
  --chip GD32H779IW --board GD32H779I_WATCH_EVAL \
  --name soft_uart_demo --version 0.1.0
```
