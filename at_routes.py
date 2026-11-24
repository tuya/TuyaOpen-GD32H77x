"""Verified ordinary-pad AT routes, GD32H779xx datasheet Rev0.5 tables 2-10 ff.

This is a deliberately bounded catalogue, not all alternate functions. Special
analog-switch pads (PC2_C/PC3_C) require separate manual verification.
"""

UART_ROUTES = {
    "USART0": {"TX": {"PA9": 7, "PB6": 7, "PF4": 4},
               "RX": {"PA10": 7, "PB7": 7, "PF5": 4}},
    "USART1": {"TX": {"PA2": 7, "PD5": 7}, "RX": {"PA3": 7, "PD6": 7}},
    "USART2": {"TX": {"PB10": 7, "PD8": 7}, "RX": {"PB11": 7, "PD9": 7}},
    "UART3": {"TX": {"PA0": 8, "PD1": 8, "PC10": 8},
              "RX": {"PA1": 8, "PD0": 8, "PC11": 8}},
    "UART4": {"TX": {"PB13": 14}, "RX": {"PB12": 14}},
    "USART5": {"TX": {"PC6": 7}, "RX": {"PC7": 7}},
    "UART6": {"TX": {"PE8": 7}, "RX": {"PE7": 7}},
    "UART7": {"TX": {"PE1": 8}, "RX": {"PE0": 8}},
}

SPI_ROUTES = {
    0: {"SCK": {"PA5": 5}, "MISO": {"PA6": 5}, "MOSI": {"PA7": 5}},
    1: {"SCK": {"PB13": 5, "PC3": 6}, "MISO": {"PB14": 5, "PC2": 5},
        "MOSI": {"PB15": 5, "PC1": 5, "PC3": 5}},
    2: {"SCK": {"PC10": 6}, "MISO": {"PC11": 6}, "MOSI": {"PC12": 6}},
    3: {"SCK": {"PE2": 5, "PE12": 5}, "MISO": {"PE5": 5, "PE13": 5},
        "MOSI": {"PE6": 5, "PE14": 5}},
    4: {"SCK": {"PF7": 5}, "MISO": {"PF8": 5}, "MOSI": {"PF9": 5}},
    5: {"SCK": {"PA5": 8}, "MISO": {"PA6": 8}, "MOSI": {"PA7": 8}},
}


def pin_binding(pin):
    """Only ordinary GPIO pads can be represented by the GPIO register API."""
    import re
    if not re.fullmatch(r"P[A-HJK](?:[0-9]|1[0-5])", pin):
        raise ValueError(f"Invalid or unsupported GPIO pad {pin!r}")
    return {"PORT": f"GPIO{pin[1]}", "CLOCK": f"RCU_GPIO{pin[1]}",
            "PIN": f"GPIO_PIN_{int(pin[2:])}"}


def signal_binding(routes, peripheral, signal, pin):
    legal = routes[peripheral][signal]
    if pin not in legal:
        raise ValueError(f"{peripheral} {signal}: unsupported route {pin}; "
                         f"verified routes: {', '.join(legal)}")
    return {**pin_binding(pin), "AF": f"GPIO_AF_{legal[pin]}"}


def validate_package(chip, pins):
    """Validate ordinary pads while allowing explicitly supported _C pads."""
    if not chip.startswith("GD32H779"):
        return  # Other variants retain their existing platform support.
    package = chip[8]
    ports = {"V": "ABCDE", "Z": "ABCDEFG", "I": "ABCDEFGH"}.get(package)
    if ports is None:
        raise ValueError(f"Unverified GD32H779 package: {chip}")
    special_pads = {"PA0_C", "PA1_C", "PC2_C", "PC3_C"}
    for pin in pins:
        if pin in special_pads:
            base = pin[:-2]
        else:
            base = pin
            pin_binding(pin)
        if pin == "PH3" or (base[1] not in ports and base not in ("PH0", "PH1")) or (
                not pin.endswith("_C") and package in "VZ" and
                base in ("PC2", "PC3")):
            raise ValueError(f"{pin} is not an ordinary bonded pad on {chip}")
