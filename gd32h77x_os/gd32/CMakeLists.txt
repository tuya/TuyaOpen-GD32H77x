add_library(gd32h77x_gd32_sdk STATIC
    CMSIS/GD/GD32H77x_78x/Source/system_gd32h77x_78x.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_rcu.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_gpio.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_adc.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_usart.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_i2c.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_dci.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_ipa.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_spi.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_timer.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_misc.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_pmu.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_syscfg.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_trng.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_dma.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_ospi.c
    GD32H77x_78x_standard_peripheral/Source/gd32h77x_78x_ospim.c
)
target_include_directories(gd32h77x_gd32_sdk PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/CMSIS
    ${CMAKE_CURRENT_LIST_DIR}/CMSIS/GD/GD32H77x_78x/Include
    ${CMAKE_CURRENT_LIST_DIR}/GD32H77x_78x_standard_peripheral/Include
)
