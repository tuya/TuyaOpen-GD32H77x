# Public TKL APIs selected from the SDK standard adapter set. Chip-private
# support sources and headers live under gd32h77x_os/main.
add_library(gd32h77x_tuyaos_adapter_headers INTERFACE)
target_include_directories(gd32h77x_tuyaos_adapter_headers INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include/system"
    "${CMAKE_CURRENT_LIST_DIR}/include/flash"
    "${CMAKE_CURRENT_LIST_DIR}/include/network"
    "${CMAKE_CURRENT_LIST_DIR}/include/security"
    "${CMAKE_CURRENT_LIST_DIR}/include/uart"
    "${CMAKE_CURRENT_LIST_DIR}/include/wifi"
    "${CMAKE_CURRENT_LIST_DIR}/include/bluetooth"
    "${CMAKE_CURRENT_LIST_DIR}/include/init/include"
    "${CMAKE_CURRENT_LIST_DIR}/include/utilities/include"
)

add_library(gd32h77x_tuyaos_adapter INTERFACE)
target_sources(gd32h77x_tuyaos_adapter INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_memory.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_mutex.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_semaphore.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_queue.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_system.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_output.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_thread.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_flash.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_uart.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_lwip.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_network.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_ota.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_sleep.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_wifi.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_wifi_hostap.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_bluetooth.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_bluetooth_bredr.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_bluetooth_mesh_device.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_bluetooth_mesh_prov.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/tkl_bluetooth_sm.c"
)
target_link_libraries(gd32h77x_tuyaos_adapter INTERFACE
    gd32h77x_tuyaos_adapter_headers
)
