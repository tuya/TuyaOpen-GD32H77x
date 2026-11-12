# Public TKL APIs selected from the SDK standard adapter set. Chip-private
# support sources and headers live under gd32h77x_os/components/.
add_library(gd32h77x_tuyaos_adapter_headers INTERFACE)
target_include_directories(gd32h77x_tuyaos_adapter_headers INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include/system"
    "${CMAKE_CURRENT_LIST_DIR}/include/flash"
    "${CMAKE_CURRENT_LIST_DIR}/include/network"
    "${CMAKE_CURRENT_LIST_DIR}/include/security"
    "${CMAKE_CURRENT_LIST_DIR}/include/uart"
    "${CMAKE_CURRENT_LIST_DIR}/include/gpio"
    "${CMAKE_CURRENT_LIST_DIR}/include/adc"
    "${CMAKE_CURRENT_LIST_DIR}/include/pwm"
    "${CMAKE_CURRENT_LIST_DIR}/include/i2c"
    "${CMAKE_CURRENT_LIST_DIR}/include/pinmux"
    "${CMAKE_CURRENT_LIST_DIR}/include/qspi"
    "${CMAKE_CURRENT_LIST_DIR}/include/spi"
    "${CMAKE_CURRENT_LIST_DIR}/include/dvp"
    "${CMAKE_CURRENT_LIST_DIR}/include/dma2d"
    "${CMAKE_CURRENT_LIST_DIR}/../../../gd32h77x_os/components/fatfs"
    "${CMAKE_CURRENT_LIST_DIR}/include/wifi"
    "${CMAKE_CURRENT_LIST_DIR}/include/bluetooth"
    "${CMAKE_CURRENT_LIST_DIR}/include/init/include"
    "${CMAKE_CURRENT_LIST_DIR}/include/utilities/include"
)

if(CONFIG_ENABLE_LIBLWIP STREQUAL "y")
    message(FATAL_ERROR "GD32H77X has no tkl_lwip backend; disable CONFIG_ENABLE_LIBLWIP")
endif()

add_library(gd32h77x_tuyaos_adapter INTERFACE)
file(GLOB TUYAOS_UTILITY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/include/utilities/src/*.c")
target_sources(gd32h77x_tuyaos_adapter INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tuyaopen_license.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_memory.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_mutex.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_semaphore.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_queue.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_system.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_output.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_thread.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_ota.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_sleep.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_network.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_flash.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_pinmux.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_uart.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_gpio.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_spi.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_qspi.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_i2c.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_adc.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_pwm.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_dvp.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_dma2d.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_wifi.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/driver/tkl_bluetooth.c"
    ${TUYAOS_UTILITY_SOURCES}
)

target_sources(gd32h77x_tuyaos_adapter INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/src/system/tkl_fs.c"
)

target_link_libraries(gd32h77x_tuyaos_adapter INTERFACE
    gd32h77x_tuyaos_adapter_headers
)
