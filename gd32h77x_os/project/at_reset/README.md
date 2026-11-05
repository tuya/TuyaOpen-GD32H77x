# AT Module Reset Helper

Native H77 console image that exposes `reset_at`. It drives the configured AT
module NRST signal through H77 with a 20 ms active-low pulse.

Build it with the standalone GD32H77X build:

```bash
python3 platform/GD32H77X/gd32h77x_os/tools/build_project.py \
  platform/GD32H77X/gd32h77x_os/project/at_reset build \
  --chip GD32H779VW --board GD32H779V_START
```

Use this together with the attached module BOOT0 held high to enter its ROM ISP
while a host-side flash tool is waiting on the module console UART.
