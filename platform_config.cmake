# Use the current TuyaOpen adapter contract rather than duplicate its headers.
list(APPEND PLATFORM_PUBINC
    "${PLATFORM_PATH}/gd32h77x_os/components/startup"
    "${PLATFORM_PATH}/gd32h77x_os/components/runtime"
    "${PLATFORM_PATH}/gd32h77x_os/components/console"
    "${PLATFORM_PATH}/gd32h77x_os/components/drivers"
    "${PLATFORM_PATH}/gd32h77x_os/components/at"
    "${PLATFORM_PATH}/gd32h77x_os/gd32/include"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/flash"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/init/include"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/system"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/network"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/security"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/uart"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/gpio"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/adc"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/pwm"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/i2c"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/pinmux"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/qspi"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/spi"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/dvp"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/dma2d"
    "${PLATFORM_PATH}/gd32h77x_os/components/fatfs"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/wifi"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/bluetooth"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/utilities/include")

# Application libraries are configured before build_example.py starts the
# nested platform build. Generate the platform-owned header here so project
# sources and the final firmware consume the same resolved configuration.
set(GD32H77X_PREBUILD_CONFIG_DIR
    "${CMAKE_BINARY_DIR}/gd32h77x/generated/platform_config")
if(TOS_PYTHON)
    set(GD32H77X_PREBUILD_PYTHON "${TOS_PYTHON}")
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GD32H77X_PREBUILD_PYTHON "${Python3_EXECUTABLE}")
endif()
set(GD32H77X_BOARD_PROJECT_CONFIG
    "${TOP_SOURCE_DIR}/boards/GD32H77X/${PLATFORM_BOARD}/platform_config/gd32h77x.config")
set(GD32H77X_PREBUILD_PROJECT_CONFIG
    "${CMAKE_BINARY_DIR}/gd32h77x/project/tuya/project.config")
set(GD32H77X_PROJECT_CONFIG_STAGER
    "${PLATFORM_PATH}/tuya_project_config.py")
file(GLOB GD32H77X_CHIP_CONFIG_INPUTS CONFIGURE_DEPENDS
    "${PLATFORM_PATH}/gd32h77x_os/chips/*/*.config")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PLATFORM_PATH}/Kconfig"
    "${PLATFORM_PATH}/platform_config.py"
    "${GD32H77X_PROJECT_CONFIG_STAGER}"
    "${GD32H77X_BOARD_PROJECT_CONFIG}"
    ${GD32H77X_CHIP_CONFIG_INPUTS})
execute_process(
    COMMAND "${GD32H77X_PREBUILD_PYTHON}"
            "${GD32H77X_PROJECT_CONFIG_STAGER}"
            --source-root "${TOP_SOURCE_DIR}"
            --board "${PLATFORM_BOARD}"
            --output "${GD32H77X_PREBUILD_PROJECT_CONFIG}"
    RESULT_VARIABLE GD32H77X_PROJECT_CONFIG_RESULT
    OUTPUT_VARIABLE GD32H77X_PROJECT_CONFIG_OUTPUT
    ERROR_VARIABLE GD32H77X_PROJECT_CONFIG_ERROR)
if(NOT GD32H77X_PROJECT_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "GD32H77X Tuya project config staging failed:
${GD32H77X_PROJECT_CONFIG_OUTPUT}${GD32H77X_PROJECT_CONFIG_ERROR}")
endif()
set(GD32H77X_PREBUILD_PROJECT_ARGS)
if(EXISTS "${GD32H77X_PREBUILD_PROJECT_CONFIG}")
    list(APPEND GD32H77X_PREBUILD_PROJECT_ARGS
        --project-config "${GD32H77X_PREBUILD_PROJECT_CONFIG}")
endif()
execute_process(
    COMMAND "${GD32H77X_PREBUILD_PYTHON}"
            "${PLATFORM_PATH}/platform_config.py"
            --platform-root "${PLATFORM_PATH}"
            --chip "${PLATFORM_CHIP}"
            --board "${PLATFORM_BOARD}"
            ${GD32H77X_PREBUILD_PROJECT_ARGS}
            --out-dir "${GD32H77X_PREBUILD_CONFIG_DIR}"
    RESULT_VARIABLE GD32H77X_PREBUILD_CONFIG_RESULT
    OUTPUT_VARIABLE GD32H77X_PREBUILD_CONFIG_OUTPUT
    ERROR_VARIABLE GD32H77X_PREBUILD_CONFIG_ERROR)
if(NOT GD32H77X_PREBUILD_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "GD32H77X prebuild configuration failed:
${GD32H77X_PREBUILD_CONFIG_OUTPUT}${GD32H77X_PREBUILD_CONFIG_ERROR}")
endif()
list(APPEND PLATFORM_PUBINC "${GD32H77X_PREBUILD_CONFIG_DIR}/include")
