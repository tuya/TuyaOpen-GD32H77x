"""Verified OSPIM PSRAM routes for GD32H77X.

The catalogue is intentionally bounded to pins verified against the
GD32H779xx datasheet alternate-function tables. Add a route only together with
a board-level electrical review.
"""

import re

# OSPIM port -> signal -> verified MCU pin -> AF number.
OSPIM_ROUTES = {
    0: {
        "CS": {"PE11": 11},
        "SCK": {"PB2": 9, "PA3": 12},
        "IO0": {"PA2": 6},
        "IO1": {"PB0": 4},
        "IO2": {"PC2_C": 9, "PA3": 6},
        "IO3": {"PA1": 9, "PA6": 6},
    },
    # Port 1 routes will be added only after the same datasheet/board review.
    1: {},
}

_SPECIAL_PADS = {
    "PA0_C": ("GPIOA", "RCU_GPIOA", 0),
    "PA1_C": ("GPIOA", "RCU_GPIOA", 1),
    "PC2_C": ("GPIOC", "RCU_GPIOC", 2),
    "PC3_C": ("GPIOC", "RCU_GPIOC", 3),
}


def ospi_pin_binding(pin: str) -> dict[str, str]:
    """Return GPIO register bindings for ordinary or _C GPIO pads."""
    if pin in _SPECIAL_PADS:
        port, clock, number = _SPECIAL_PADS[pin]
        return {"PORT": port, "CLOCK": clock, "PIN": f"GPIO_PIN_{number}"}

    if not re.fullmatch(r"P[A-HJK](?:[0-9]|1[0-5])", pin):
        raise ValueError(f"Invalid or unsupported GPIO pad {pin!r}")
    return {
        "PORT": f"GPIO{pin[1]}",
        "CLOCK": f"RCU_GPIO{pin[1]}",
        "PIN": f"GPIO_PIN_{int(pin[2:])}",
    }


def ospi_signal_binding(port: int, signal: str, pin: str) -> dict[str, str]:
    """Resolve one OSPIM signal route and its verified alternate function."""
    if port not in OSPIM_ROUTES:
        raise ValueError(f"Unsupported OSPIM port {port}")
    routes = OSPIM_ROUTES[port].get(signal)
    if routes is None:
        raise ValueError(f"Unsupported OSPIM signal {signal}")
    if pin not in routes:
        legal = ", ".join(routes) if routes else "none"
        raise ValueError(
            f"OSPIM port {port} {signal}: unsupported route {pin}; "
            f"verified routes: {legal}")
    return {**ospi_pin_binding(pin), "AF": f"GPIO_AF_{routes[pin]}"}


def validate_ospi_package(chip: str, pins: list[str]) -> None:
    """Validate that the package exposes every selected PSRAM pad."""
    if not chip.startswith("GD32H779"):
        return
    package = chip[8]
    ports = {"V": "ABCDE", "Z": "ABCDEFG", "I": "ABCDEFGH"}.get(package)
    if ports is None:
        raise ValueError(f"Unverified GD32H779 package: {chip}")
    for pin in pins:
        ospi_pin_binding(pin)
        base = pin[:-2] if pin.endswith("_C") else pin
        if base[1] not in ports:
            raise ValueError(f"{pin} is not bonded on {chip}")
        if not pin.endswith("_C") and package in "VZ" and base in ("PC2", "PC3"):
            raise ValueError(f"{pin} is not an ordinary bonded pad on {chip}")
