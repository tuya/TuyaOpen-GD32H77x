cmake_minimum_required(VERSION 3.20)
project(gd32h77x_host_tests C)
enable_testing()

# The VW553 platform repository is an optional sibling checkout. Register the
# cross-end protocol test only when its portable protocol helper is available.
set(GD32H77X_VW553_SPI_PROTOCOL
    "${CMAKE_CURRENT_SOURCE_DIR}/../../GD32/gd32_os/MSDK/vw553_at_gateway/gateway_spi_protocol.c")
if(EXISTS "${GD32H77X_VW553_SPI_PROTOCOL}")
    add_test(NAME gd32h77x_spi_at_integration
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/spi_at_integration/run.sh")
endif()

option(ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" ON)
set(VW553_TCP_OPTIONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../GD32/gd32_os/MSDK/vw553_at_gateway")
if(EXISTS "${VW553_TCP_OPTIONS_DIR}/gateway_tcp_options.c")
    add_executable(test_vw553_tcp_options test_vw553_tcp_options.c "${VW553_TCP_OPTIONS_DIR}/gateway_tcp_options.c")
    target_include_directories(test_vw553_tcp_options PRIVATE "${VW553_TCP_OPTIONS_DIR}")
    target_compile_definitions(test_vw553_tcp_options PRIVATE GATEWAY_TCP_OPTIONS_HOST_TEST=1)
    add_test(NAME vw553_tcp_options_readback COMMAND test_vw553_tcp_options)
    add_executable(test_vw553_tcp_option_masks test_vw553_tcp_option_masks.c)
    target_include_directories(test_vw553_tcp_option_masks PRIVATE "${VW553_TCP_OPTIONS_DIR}")
    target_compile_definitions(test_vw553_tcp_option_masks PRIVATE
        GATEWAY_TCP_OPTIONS_SOURCE="${VW553_TCP_OPTIONS_DIR}/gateway_tcp_options.c")
    add_test(NAME vw553_tcp_option_masks COMMAND test_vw553_tcp_option_masks)
endif()
if(TOS_PYTHON)
    set(GD32H77X_TEST_PYTHON "${TOS_PYTHON}")
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GD32H77X_TEST_PYTHON "${Python3_EXECUTABLE}")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../GD32/gd32_os/MSDK/vw553_at_gateway/ble/atcmd_ble.c")
    add_test(NAME gd32h77x_ble_urc_interleave
        COMMAND "${GD32H77X_TEST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/ble_urc_host/run.py")
endif()
set(GD32H77X_TEST_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/platform_config")
set(GD32H77X_OPEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(GD32H77X_COMPONENTS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../gd32h77x_os/components")
set(GD32H77X_TEST_PROJECT_CONFIG
    "${CMAKE_CURRENT_BINARY_DIR}/project/tuya/project.config")
execute_process(
    COMMAND "${GD32H77X_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tuya_project_config.py"
            --source-root "${GD32H77X_OPEN_ROOT}"
            --board GD32H779V_START
            --output "${GD32H77X_TEST_PROJECT_CONFIG}"
    RESULT_VARIABLE GD32H77X_TEST_PROJECT_CONFIG_RESULT
    OUTPUT_VARIABLE GD32H77X_TEST_PROJECT_CONFIG_OUTPUT
    ERROR_VARIABLE GD32H77X_TEST_PROJECT_CONFIG_ERROR)
if(NOT GD32H77X_TEST_PROJECT_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot stage GD32H77X test project config:
${GD32H77X_TEST_PROJECT_CONFIG_OUTPUT}${GD32H77X_TEST_PROJECT_CONFIG_ERROR}")
endif()
set(GD32H77X_TEST_PROJECT_CONFIG_ARGS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../at_routes.py")
if(EXISTS "${GD32H77X_TEST_PROJECT_CONFIG}")
    list(APPEND GD32H77X_TEST_PROJECT_CONFIG_ARGS
        --project-config "${GD32H77X_TEST_PROJECT_CONFIG}")
endif()
execute_process(
    COMMAND "${GD32H77X_TEST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/../platform_config.py"
            --platform-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
            --chip GD32H779VW --board GD32H779V_START
            ${GD32H77X_TEST_PROJECT_CONFIG_ARGS}
            --out-dir "${GD32H77X_TEST_CONFIG_DIR}"
    RESULT_VARIABLE GD32H77X_TEST_CONFIG_RESULT
    OUTPUT_VARIABLE GD32H77X_TEST_CONFIG_OUTPUT
    ERROR_VARIABLE GD32H77X_TEST_CONFIG_ERROR)
if(NOT GD32H77X_TEST_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot generate GD32H77X test config:
${GD32H77X_TEST_CONFIG_OUTPUT}${GD32H77X_TEST_CONFIG_ERROR}")
endif()
include("${GD32H77X_TEST_CONFIG_DIR}/gd32h77x_config.cmake")
include_directories("${GD32H77X_TEST_CONFIG_DIR}/include")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../gd32h77x_os/gd32/include")
set(GD32H77X_AT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../gd32h77x_os/components/at")
add_subdirectory(../tuyaos/tuyaos_adapter "${CMAKE_CURRENT_BINARY_DIR}/tuyaos_adapter")

add_executable(test_at_parser test_at_parser.c ../gd32h77x_os/components/at/at_parser.c)
target_include_directories(test_at_parser PRIVATE ${GD32H77X_AT_ROOT})
add_test(NAME at_binary_parser COMMAND test_at_parser)

add_executable(test_at_client test_at_client.c ../gd32h77x_os/components/at/at_client.c
    ../gd32h77x_os/components/at/at_parser.c
    ../gd32h77x_os/components/at/at_profile.c)
target_include_directories(test_at_client PRIVATE ${GD32H77X_AT_ROOT})
add_test(NAME at_transaction_client COMMAND test_at_client)

add_executable(test_event_queue test_event_queue.c
    ../gd32h77x_os/components/at/at_event_queue.c
    ../gd32h77x_os/components/at/at_client.c
    ../gd32h77x_os/components/at/at_parser.c
    ../gd32h77x_os/components/at/at_profile.c)
target_include_directories(test_event_queue PRIVATE ${GD32H77X_AT_ROOT})
add_test(NAME at_event_queue_stress COMMAND test_event_queue)

add_executable(test_socket test_socket.c
    ../gd32h77x_os/components/at/at_socket.c)
target_include_directories(test_socket PRIVATE
    socket_fakes
    fixtures
    ${GD32H77X_AT_ROOT})
target_compile_definitions(test_socket PRIVATE GD32H77X_AT_SOCKET_TEST)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # ARM may pack at_socket_errno_t into one signed byte. Reproduce that ABI
    # condition on the host so byte counts above 127 cannot regress silently.
    target_compile_options(test_socket PRIVATE -fshort-enums)
endif()
add_test(NAME at_socket_semantics COMMAND test_socket)

add_executable(test_profile test_profile.c
    ../gd32h77x_os/components/at/at_profile.c)
target_include_directories(test_profile PRIVATE ${GD32H77X_AT_ROOT})
add_test(NAME at_profile_policy COMMAND test_profile)

add_executable(test_module_reset test_module_reset.c
    ../gd32h77x_os/components/at/at_module_reset.c)
target_include_directories(test_module_reset PRIVATE ${GD32H77X_AT_ROOT})
add_test(NAME at_module_reset_sequence COMMAND test_module_reset)

add_executable(test_tkl_network_basics test_tkl_network_basics.c
    ../tuyaos/tuyaos_adapter/src/system/tkl_network.c)
target_include_directories(test_tkl_network_basics PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    "${CMAKE_CURRENT_SOURCE_DIR}/socket_fakes"
    ${GD32H77X_AT_ROOT}
    ../gd32h77x_os/gd32/CMSIS/GD/GD32H77x_78x/Include
    ../tuyaos/tuyaos_adapter/include/network
    ../tuyaos/tuyaos_adapter/include/system
    ../tuyaos/tuyaos_adapter/include/utilities/include
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/common/include")
target_compile_definitions(test_tkl_network_basics PRIVATE TUYA_FD_MAX_COUNT=64)
target_compile_options(test_tkl_network_basics PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_tkl_network_basics PRIVATE -Wl,--gc-sections)
add_test(NAME tkl_network_common_semantics COMMAND test_tkl_network_basics)


add_executable(test_tkl_wifi test_tkl_wifi.c
    ../tuyaos/tuyaos_adapter/src/driver/tkl_wifi.c)
target_include_directories(test_tkl_wifi PRIVATE
    wifi_fakes
    ${GD32H77X_AT_ROOT}
    ../gd32h77x_os/gd32/CMSIS
    ../gd32h77x_os/gd32/CMSIS/GD/GD32H77x_78x/Include
    ../gd32h77x_os/gd32/GD32H77x_78x_standard_peripheral/Include
    ../tuyaos/tuyaos_adapter/include/wifi
    ../tuyaos/tuyaos_adapter/include/system
    ../tuyaos/tuyaos_adapter/include/utilities/include
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/tal_system/include")
target_compile_definitions(test_tkl_wifi PRIVATE GD32H77DXW USE_STDPERIPH_DRIVER GD32H77X_TKL_WIFI_TEST)
add_test(NAME tkl_wifi_response_contract COMMAND test_tkl_wifi)

set(TUYA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

add_executable(test_http_response_content_length
    test_http_response_content_length.c
    "${TUYA_ROOT}/src/libhttp/coreHTTP/source/core_http_client.c"
    "${TUYA_ROOT}/src/libhttp/coreHTTP/source/dependency/3rdparty/http_parser/http_parser.c")
target_include_directories(test_http_response_content_length PRIVATE
    http_fakes
    "${TUYA_ROOT}/src/libhttp/coreHTTP"
    "${TUYA_ROOT}/src/libhttp/coreHTTP/source/include"
    "${TUYA_ROOT}/src/libhttp/coreHTTP/source/dependency/3rdparty/http_parser"
    "${TUYA_ROOT}/src/libhttp/include"
    "${TUYA_ROOT}/src/libmqtt/include"
    "${TUYA_ROOT}/src/tal_system/include"
    "${TUYA_ROOT}/src/tal_network/include"
    "${TUYA_ROOT}/src/tal_driver/uart"
    "${TUYA_ROOT}/src/tal_kv/include"
    "${TUYA_ROOT}/src/tal_kv/littlefs"
    "${TUYA_ROOT}/src/tal_wifi/include"
    "${TUYA_ROOT}/src/tal_bluetooth/include"
    "${TUYA_ROOT}/src/tal_cli/include"
    "${TUYA_ROOT}/src/tal_image/include"
    "${TUYA_ROOT}/src/tal_security/include"
    "${TUYA_ROOT}/src/libtls/include"
    "${TUYA_ROOT}/src/libtls/port"
    "${TUYA_ROOT}/src/libtls/mbedtls-3.1.0/include"
    "${TUYA_ROOT}/src/common/include"
    "${TUYA_ROOT}/src/common/utilities"
    "${TUYA_ROOT}/src/tuya_cloud_service/transport"
    "${TUYA_ROOT}/src/tuya_cloud_service/tls"
    ../tuyaos/tuyaos_adapter/include/utilities/include
    ../tuyaos/tuyaos_adapter/include/security
    ../tuyaos/tuyaos_adapter/include/system
    ../tuyaos/tuyaos_adapter/include/network
    ../tuyaos/tuyaos_adapter/include/uart
    ../tuyaos/tuyaos_adapter/include/wifi
    ../tuyaos/tuyaos_adapter/include/bluetooth
    ../tuyaos/tuyaos_adapter/include/flash
    ../tuyaos/tuyaos_adapter/include/init/include)
set_source_files_properties("${TUYA_ROOT}/src/libhttp/coreHTTP/source/core_http_client.c"
    PROPERTIES COMPILE_OPTIONS "-w")
add_test(NAME http_content_length_semantics COMMAND test_http_response_content_length)
set(GD32H77X_TEST_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../gd32h77x_os/tools/firmware/resolve_memory_layout.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/resolve_host_layout.py")
set(GD32H77X_HOST_LAYOUT "${CMAKE_CURRENT_BINARY_DIR}/generated-layout")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/resolve_host_layout.py"
            --platform-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
            --boards-root "${TUYA_ROOT}/boards"
            --out-dir "${GD32H77X_HOST_LAYOUT}"
    RESULT_VARIABLE GD32H77X_HOST_LAYOUT_RESULT
    OUTPUT_VARIABLE GD32H77X_HOST_LAYOUT_OUTPUT
    ERROR_VARIABLE GD32H77X_HOST_LAYOUT_ERROR)
if(NOT GD32H77X_HOST_LAYOUT_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot resolve GD32H77X host-test layout:\n${GD32H77X_HOST_LAYOUT_OUTPUT}${GD32H77X_HOST_LAYOUT_ERROR}")
endif()
set(GD32H77X_H779_HOST_LAYOUT "${CMAKE_CURRENT_BINARY_DIR}/generated-layout-h779")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/resolve_host_layout.py"
            --platform-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
            --boards-root "${TUYA_ROOT}/boards"
            --chip GD32H779VW --board GD32H779V_START
            --out-dir "${GD32H77X_H779_HOST_LAYOUT}"
    RESULT_VARIABLE GD32H77X_H779_LAYOUT_RESULT
    OUTPUT_VARIABLE GD32H77X_H779_LAYOUT_OUTPUT
    ERROR_VARIABLE GD32H77X_H779_LAYOUT_ERROR)
if(NOT GD32H77X_H779_LAYOUT_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot resolve GD32H779VW host-test layout:\n${GD32H77X_H779_LAYOUT_OUTPUT}${GD32H77X_H779_LAYOUT_ERROR}")
endif()
add_executable(test_storage_bounds test_storage_bounds.c
    ../tuyaos/tuyaos_adapter/src/driver/tkl_flash.c
    ../gd32h77x_os/components/drivers/gd32h77x_flash_route.c mock_cnvm.c mock_ecnvm.c mock_nor.c
    fixtures/gd32h77x_memory_layout.c)
target_include_directories(test_storage_bounds PRIVATE
    "${GD32H77X_TEST_FIXTURES}"
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers
    "${TUYA_ROOT}/src/common/include"
)
target_link_libraries(test_storage_bounds PRIVATE gd32h77x_tuyaos_adapter_headers)
add_test(NAME gd32h77x_storage_bounds COMMAND test_storage_bounds)

add_executable(test_flash_info_partial test_flash_info_partial.c
    ../tuyaos/tuyaos_adapter/src/driver/tkl_flash.c
    ../gd32h77x_os/components/drivers/gd32h77x_flash_route.c mock_cnvm.c mock_ecnvm.c mock_nor.c
    fixtures/gd32h77x_memory_layout.c)
target_compile_definitions(test_flash_info_partial PRIVATE GD32H77X_TEST_CNVM_BACKEND_PARTIAL)
target_include_directories(test_flash_info_partial PRIVATE
    "${GD32H77X_TEST_FIXTURES}"
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers
    "${TUYA_ROOT}/src/common/include"
)
target_link_libraries(test_flash_info_partial PRIVATE gd32h77x_tuyaos_adapter_headers)
add_test(NAME gd32h77x_flash_info_partial COMMAND test_flash_info_partial)

add_executable(test_flash_routing test_flash_routing.c
    ../gd32h77x_os/components/drivers/gd32h77x_flash_route.c
    fixtures/gd32h77x_memory_layout.c)
target_include_directories(test_flash_routing PRIVATE "${GD32H77X_TEST_FIXTURES}"
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers)
target_link_libraries(test_flash_routing PRIVATE gd32h77x_tuyaos_adapter_headers)
add_test(NAME gd32h77x_flash_routing COMMAND test_flash_routing)

add_executable(test_flash_layout_generated test_flash_layout_generated.c
    ../gd32h77x_os/components/drivers/gd32h77x_flash_route.c
    "${GD32H77X_HOST_LAYOUT}/gd32h77x_memory_layout.c")
target_include_directories(test_flash_layout_generated PRIVATE "${GD32H77X_HOST_LAYOUT}/include"
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers)
target_link_libraries(test_flash_layout_generated PRIVATE gd32h77x_tuyaos_adapter_headers)
add_test(NAME gd32h77x_flash_layout_generated COMMAND test_flash_layout_generated)

add_executable(test_ecnvm_flash_layout test_ecnvm_flash_layout.c
    ../tuyaos/tuyaos_adapter/src/driver/tkl_flash.c
    ../gd32h77x_os/components/drivers/gd32h77x_flash_route.c mock_cnvm.c mock_ecnvm.c mock_nor.c
    "${GD32H77X_H779_HOST_LAYOUT}/gd32h77x_memory_layout.c")
target_include_directories(test_ecnvm_flash_layout PRIVATE
    "${GD32H77X_H779_HOST_LAYOUT}/include"
    "${GD32H77X_TEST_FIXTURES}"
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers
    "${TUYA_ROOT}/src/common/include")
target_compile_definitions(test_ecnvm_flash_layout PRIVATE GD32H77X_ECNVM_HOST_TEST)
target_link_libraries(test_ecnvm_flash_layout PRIVATE gd32h77x_tuyaos_adapter_headers)
add_test(NAME gd32h77x_ecnvm_kv_layout COMMAND test_ecnvm_flash_layout)

add_executable(test_nor_protocol test_nor_protocol.c ../gd32h77x_os/components/drivers/gd32h77x_nor.c
    fixtures/gd32h77x_memory_layout.c)
target_compile_definitions(test_nor_protocol PRIVATE GD32H77X_NOR_HOST_TEST)
target_include_directories(test_nor_protocol PRIVATE "${GD32H77X_TEST_FIXTURES}"
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers)
add_test(NAME gd32h77x_nor_protocol COMMAND test_nor_protocol)

find_package(Threads REQUIRED)
add_executable(test_entropy test_entropy.c ../gd32h77x_os/components/drivers/gd32h77x_entropy.c)
target_compile_definitions(test_entropy PRIVATE GD32H77X_ENTROPY_TEST)
target_include_directories(test_entropy PRIVATE
    ${GD32H77X_COMPONENTS_ROOT}/runtime ${GD32H77X_COMPONENTS_ROOT}/drivers
    "${TUYA_ROOT}/src/common/include"
)
target_link_libraries(test_entropy PRIVATE Threads::Threads)
add_test(NAME gd32h77x_entropy_state COMMAND test_entropy)
add_test(NAME gd32h77x_tkl_random_adapter
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_tkl_random_source.py")

set(GD32H77X_HOST_TESTS test_at_parser test_at_client test_socket test_profile test_module_reset test_tkl_network_basics test_storage_bounds test_flash_info_partial
    test_flash_routing test_flash_layout_generated test_ecnvm_flash_layout test_nor_protocol test_entropy test_http_response_content_length)
if(TARGET test_vw553_tcp_options)
    list(APPEND GD32H77X_HOST_TESTS test_vw553_tcp_options test_vw553_tcp_option_masks)
endif()
foreach(test_target IN LISTS GD32H77X_HOST_TESTS)
    target_compile_options(${test_target} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    if(ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()
target_compile_options(test_tkl_network_basics PRIVATE -Wno-unused-parameter)

# Guard the recorded AT firmware delta: binary payloads must never be emitted
# through the vendor printf, which truncates at NUL bytes.
add_test(NAME at_payload_encoding
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_payload_encoding.py")
add_test(NAME at_cip_lifecycle
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cip_lifecycle.py")
add_test(NAME at_ble_security_policy
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ble_security.py")
add_test(NAME gd32h77x_board_gd32h77_config_layout
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_board_gd32h77_config_layout.py")
add_test(NAME gd32h77x_native_project_build
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_native_project_build.py")
add_test(NAME gd32h77x_build_example
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_build_example.py")
add_test(NAME gd32h77x_layout_csv_format
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_format_layout_csv.py")
add_test(NAME gd32h77x_rtos_heap
    COMMAND "${GD32H77X_TEST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/test_rtos_heap.py")
find_program(GD32H77X_TEST_ARM_GCC arm-none-eabi-gcc
    HINTS /opt/gcc-arm-none-eabi-10.3-2021.10/bin)
if(GD32H77X_TEST_ARM_GCC)
    set_tests_properties(gd32h77x_rtos_heap PROPERTIES
        ENVIRONMENT "ARM_NONE_EABI_GCC=${GD32H77X_TEST_ARM_GCC}")
    add_test(NAME gd32h77x_uart_isp
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_uart_isp.py")
    set_tests_properties(gd32h77x_uart_isp PROPERTIES
        ENVIRONMENT "ARM_NONE_EABI_GCC=${GD32H77X_TEST_ARM_GCC}")
endif()
add_test(NAME gd32h77x_platform_config
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_platform_config.py")
add_test(NAME gd32h77x_wireless_fw
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_wireless_fw.py")
add_test(NAME gd32h77x_spi_irq_host
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/spi_irq_host/run.sh")

# Optional real-source regression against an external AT SDK candidate.
# The default host suite must remain independent of SDK staging paths.
set(AT_SDK_ROOT "" CACHE PATH "Optional AT SDK root for production-source regression")
if(AT_SDK_ROOT)
    add_test(NAME at_candidate_production_regression
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_candidate_source.py"
                --candidate-root "${AT_SDK_ROOT}")
endif()
