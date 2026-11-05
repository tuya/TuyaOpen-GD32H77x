##
# @file CMakeLists.txt
# @brief GD32H77X-owned board diagnostic CLI example.
#
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(GD32H77X_ROOT ${CMAKE_SOURCE_DIR}/platform/GD32H77X)
get_filename_component(APP_NAME ${APP_PATH} NAME)
aux_source_directory(${APP_PATH}/src APP_SRCS)
list(APPEND APP_SRCS
    ${APP_PATH}/adapter_diag_cli.c
    ${APP_PATH}/at_probe.c
)
add_library(${EXAMPLE_LIB})
target_sources(${EXAMPLE_LIB} PRIVATE ${APP_SRCS})
target_include_directories(${EXAMPLE_LIB} PRIVATE
    ${APP_PATH}
    ${GD32H77X_ROOT}/gd32h77x_os/components/startup
    ${GD32H77X_ROOT}/gd32h77x_os/components/runtime
    ${GD32H77X_ROOT}/gd32h77x_os/components/console
    ${GD32H77X_ROOT}/gd32h77x_os/components/drivers
    ${GD32H77X_ROOT}/tuyaos/tuyaos_adapter/include/flash
    ${GD32H77X_ROOT}/gd32h77x_os/components/at
    ${GD32H77X_ROOT}/gd32h77x_os/gd32/include
    ${GD32H77X_ROOT}/gd32h77x_os/gd32/CMSIS
    ${GD32H77X_ROOT}/gd32h77x_os/gd32/CMSIS/GD/GD32H77x_78x/Include
    ${GD32H77X_ROOT}/gd32h77x_os/gd32/GD32H77x_78x_standard_peripheral/Include
    ${GD32H77X_ROOT}/gd32h77x_os/components/FreeRTOS/include
    ${GD32H77X_ROOT}/gd32h77x_os/components/FreeRTOS/portable/GCC/ARM_CM7/r0p1
    ${CMAKE_BINARY_DIR}/gd32h77x/generated/memory/include
    ${CMAKE_BINARY_DIR}/gd32h77x/generated/platform_config/include
    ${GD32H77X_ROOT}/tests/generated-layout/include
)
target_compile_definitions(${EXAMPLE_LIB} PRIVATE
    GD32H779XW
    USE_STDPERIPH_DRIVER
    HXTAL_VALUE=25000000U
)
