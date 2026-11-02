cmake_minimum_required(VERSION 3.20)
project(gd32h77x_firmware LANGUAGES C CXX ASM)

include("${BUILD_PARAM_PATH}/build_param.cmake")
if(PLATFORM_CHIP STREQUAL "GD32H77D" AND PLATFORM_BOARD STREQUAL "GD32H77D_GINO")
    set(GD32H77X_DEVICE_DEFINE GD32H77DXW)
    set(GD32H77X_PMU_DEFINE SEL_PMU_SMPS_MODE=PMU_BYPASS)
elseif(PLATFORM_CHIP STREQUAL "GD32H779VW" AND PLATFORM_BOARD STREQUAL "GD32H779V_START")
    set(GD32H77X_DEVICE_DEFINE GD32H779XW)
else()
    message(FATAL_ERROR
        "Unsupported GD32H77X board/chip combination: ${PLATFORM_BOARD}/${PLATFORM_CHIP}")
endif()
# Adapter capability selection is independent of runtime completeness. Each
# TKL implementation remains fail-closed until its actual backend is available.
set(GD "${CMAKE_CURRENT_SOURCE_DIR}/gd32h77x_os/gd32")
set(CMSIS "${GD}/CMSIS/GD/GD32H77x_78x")
set(PERIPH "${GD}/GD32H77x_78x_standard_peripheral")
set(RTOS "${CMAKE_CURRENT_SOURCE_DIR}/gd32h77x_os/FreeRTOS")
set(PORT "${RTOS}/portable/GCC/ARM_CM7/r0p1")
# Resolve the selected platform default or complete board CSV override during
# configure, so every build consumes one layout snapshot for C, linker and
# image verification. Watch both board CSV paths for content and presence
# changes in a warm build tree.
set(GD32H77X_LAYOUT_RESOLVER "${CMAKE_CURRENT_SOURCE_DIR}/gd32h77x_os/tools/firmware/resolve_memory_layout.py")
set(GD32H77X_CHIPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gd32h77x_os/chips")
set(GD32H77X_GEOMETRIES "${GD32H77X_CHIPS_ROOT}/geometries.json")
set(GD32H77X_PLATFORM_CHIPS "${GD32H77X_CHIPS_ROOT}/platform_chips.json")
# The registry key is the official DFP device name (for example GD32H77DIW),
# not a board- or capacity-derived name. Keep this in sync with the resolver.
file(READ "${GD32H77X_PLATFORM_CHIPS}" GD32H77X_CHIPS_JSON)
string(JSON GD32H77X_TARGET_CHIP ERROR_VARIABLE GD32H77X_CHIP_ERROR
    GET "${GD32H77X_CHIPS_JSON}" platform_chips "${PLATFORM_CHIP}" target_chip)
if(GD32H77X_CHIP_ERROR)
    message(FATAL_ERROR "No memory layout registered for chip ${PLATFORM_CHIP}: ${GD32H77X_CHIP_ERROR}")
endif()
set(LINKER_TEMPLATE "${GD32H77X_CHIPS_ROOT}/${GD32H77X_TARGET_CHIP}/linker.ld")
if(NOT EXISTS "${LINKER_TEMPLATE}")
    message(FATAL_ERROR
        "No linker template is available for canonical chip ${GD32H77X_TARGET_CHIP}: ${LINKER_TEMPLATE}")
endif()
set(GD32H77X_DEFAULT_PARTITIONS "${GD32H77X_CHIPS_ROOT}/${GD32H77X_TARGET_CHIP}/partitions.csv")
set(GD32H77X_DEFAULT_DEVICES "${GD32H77X_CHIPS_ROOT}/${GD32H77X_TARGET_CHIP}/flash_devices.csv")
set(GD32H77X_BOARD_DIR "${OPEN_ROOT}/boards/GD32H77X/${PLATFORM_BOARD}")
set(GD32H77X_BOARD_PARTITIONS "${GD32H77X_BOARD_DIR}/partitions.csv")
set(GD32H77X_BOARD_DEVICES "${GD32H77X_BOARD_DIR}/flash_devices.csv")
file(GLOB GD32H77X_BOARD_LAYOUT_STATE CONFIGURE_DEPENDS
    "${GD32H77X_BOARD_PARTITIONS}" "${GD32H77X_BOARD_DEVICES}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GD32H77X_LAYOUT_RESOLVER}" "${GD32H77X_DEFAULT_PARTITIONS}"
    "${GD32H77X_DEFAULT_DEVICES}"
    "${GD32H77X_GEOMETRIES}" "${GD32H77X_PLATFORM_CHIPS}" "${LINKER}"
    "${BUILD_PARAM_PATH}/build_param.json")
if(EXISTS "${GD32H77X_BOARD_PARTITIONS}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GD32H77X_BOARD_PARTITIONS}")
endif()
if(EXISTS "${GD32H77X_BOARD_DEVICES}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GD32H77X_BOARD_DEVICES}")
endif()
set(GD32H77X_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/memory")
if(TOS_PYTHON)
    set(GD32H77X_PYTHON "${TOS_PYTHON}")
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GD32H77X_PYTHON "${Python3_EXECUTABLE}")
endif()
execute_process(
    COMMAND "${GD32H77X_PYTHON}" "${GD32H77X_LAYOUT_RESOLVER}"
            --build-params "${BUILD_PARAM_PATH}" --out-dir "${GD32H77X_GENERATED_DIR}"
    RESULT_VARIABLE GD32H77X_LAYOUT_RESULT
    OUTPUT_VARIABLE GD32H77X_LAYOUT_OUTPUT
    ERROR_VARIABLE GD32H77X_LAYOUT_ERROR)
if(NOT GD32H77X_LAYOUT_RESULT EQUAL 0)
    message(FATAL_ERROR "GD32H77X flash layout resolution failed:\n${GD32H77X_LAYOUT_OUTPUT}${GD32H77X_LAYOUT_ERROR}")
endif()
set(GD32H77X_MEMORY_INCLUDE "${GD32H77X_GENERATED_DIR}/gd32h77x_memory.ldinc")
set(GD32H77X_LAYOUT_HEADER "${GD32H77X_GENERATED_DIR}/include/gd32h77x_memory_layout.h")
set(GD32H77X_LAYOUT_SOURCE "${GD32H77X_GENERATED_DIR}/gd32h77x_memory_layout.c")
set(GD32H77X_RESOLVED_LAYOUT "${GD32H77X_GENERATED_DIR}/resolved_layout.json")
if(CONFIG_GD32H77X_ECNVM_CODE STREQUAL "y")
    if(NOT PLATFORM_BOARD STREQUAL "GD32H779V_START")
        message(FATAL_ERROR "GD32H77X_ECNVM_CODE is supported only on GD32H779V_START")
    endif()
    # Keep the boot table and vector table in CNVM. Move the application
    # instruction stream and read-only data into executable ECNVM.
    set(CNVM_TEXT_INPUTS "")
    set(ECNVM_OUTPUT_SECTIONS ".ecnvm_text : { *(.text*) *(.rodata*) } > ECNVM\n    ASSERT(SIZEOF(.ecnvm_text) > 0, \"ECNVM_CODE enabled but ECNVM text is empty\")")
else()
    set(CNVM_TEXT_INPUTS "*(.text*) *(.rodata*)")
    set(ECNVM_OUTPUT_SECTIONS "")
endif()
set(LINKER "${GD32H77X_GENERATED_DIR}/gd32h77x_linker.ld")
configure_file("${LINKER_TEMPLATE}" "${LINKER}" @ONLY)
set(GD32H77X_LAYOUT_DEPENDENCIES "${GD32H77X_LAYOUT_RESOLVER}"
    "${GD32H77X_DEFAULT_PARTITIONS}" "${GD32H77X_DEFAULT_DEVICES}"
    "${GD32H77X_GEOMETRIES}" "${GD32H77X_PLATFORM_CHIPS}" "${LINKER_TEMPLATE}"
    "${BUILD_PARAM_PATH}/build_param.json")
if(EXISTS "${GD32H77X_BOARD_PARTITIONS}")
    list(APPEND GD32H77X_LAYOUT_DEPENDENCIES "${GD32H77X_BOARD_PARTITIONS}")
endif()
if(EXISTS "${GD32H77X_BOARD_DEVICES}")
    list(APPEND GD32H77X_LAYOUT_DEPENDENCIES "${GD32H77X_BOARD_DEVICES}")
endif()
add_custom_command(
    OUTPUT "${GD32H77X_MEMORY_INCLUDE}" "${GD32H77X_LAYOUT_HEADER}"
           "${GD32H77X_LAYOUT_SOURCE}" "${GD32H77X_RESOLVED_LAYOUT}"
    COMMAND "${GD32H77X_PYTHON}" "${GD32H77X_LAYOUT_RESOLVER}"
            --build-params "${BUILD_PARAM_PATH}" --out-dir "${GD32H77X_GENERATED_DIR}"
    DEPENDS ${GD32H77X_LAYOUT_DEPENDENCIES}
    COMMENT "Regenerating resolved GD32H77X memory layout"
    VERBATIM)

add_subdirectory(tuyaos/tuyaos_adapter)
file(GLOB UTILITY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tuyaos/tuyaos_adapter/include/utilities/src/*.c")
add_executable(firmware
    gd32h77x_os/main/main.c gd32h77x_os/main/gd32h77x_platform.c
    gd32h77x_os/main/syscalls.c
    gd32h77x_os/main/tuyaopen_license.c
    gd32h77x_os/main/gd32h77x_entropy.c
    gd32h77x_os/main/gd32h77x_flash_route.c
    gd32h77x_os/main/startup_gd32h77d.S
    gd32h77x_os/wireless/vw553/vw553_at_parser.c
    gd32h77x_os/wireless/vw553/vw553_at_client.c
    gd32h77x_os/wireless/vw553/vw553_event_queue.c
    gd32h77x_os/wireless/vw553/vw553_uart_policy.c
    gd32h77x_os/wireless/vw553/vw553_uart_transport.c
    gd32h77x_os/wireless/vw553/vw553_probe.c
    gd32h77x_os/wireless/vw553/vw553_device.c
    gd32h77x_os/wireless/vw553/vw553_socket_backend.c
    gd32h77x_os/main/gd32h77x_cnvm.c
    gd32h77x_os/main/gd32h77x_ecnvm.c
    gd32h77x_os/main/gd32h77x_nor.c
    "${GD32H77X_LAYOUT_SOURCE}"
    ${UTILITY_SOURCES}
    "${CMSIS}/Source/GCC/startup_gd32h77x_78x.S"
    "${CMSIS}/Source/system_gd32h77x_78x.c"
    "${RTOS}/tasks.c" "${RTOS}/queue.c" "${RTOS}/list.c" "${RTOS}/timers.c"
    "${RTOS}/event_groups.c" "${RTOS}/stream_buffer.c"
    "${RTOS}/portable/MemMang/heap_4.c" "${PORT}/port.c"
)
target_link_libraries(firmware PRIVATE gd32h77x_tuyaos_adapter)
if(CONFIG_GD32H77X_VW553_AT_BRIDGE STREQUAL "y")
    target_sources(firmware PRIVATE gd32h77x_os/main/gd32h77x_vw553_bridge.c)
    target_compile_definitions(firmware PRIVATE GD32H77X_VW553_AT_BRIDGE=1)
endif()
if(CONFIG_GD32H77X_DIAG_CLI STREQUAL "y")
    if(NOT CONFIG_ENABLE_UART STREQUAL "y" OR NOT CONFIG_ENABLE_SERIAL_CLI_CMD STREQUAL "y")
        message(FATAL_ERROR "GD32H77X_DIAG_CLI requires ENABLE_UART and ENABLE_SERIAL_CLI_CMD")
    endif()
    target_sources(firmware PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/gd32h77x_diag_cli.c")
    target_compile_definitions(firmware PRIVATE GD32H77X_DIAG_CLI=1)
endif()
foreach(driver rcu gpio usart misc pmu syscfg trng dma ospi ospim)
    target_sources(firmware PRIVATE "${PERIPH}/Source/gd32h77x_78x_${driver}.c")
endforeach()
target_include_directories(firmware PRIVATE gd32h77x_os/main gd32h77x_os/wireless/vw553
    "${GD32H77X_BOARD_DIR}"
    tests/cli "${GD32H77X_GENERATED_DIR}/include" "${GD}/CMSIS" "${CMSIS}/Include"
    "${PERIPH}/Include" "${RTOS}/include" "${PORT}")
separate_arguments(OPEN_INCLUDES NATIVE_COMMAND "${OPEN_HEADER_DIR}")
target_include_directories(firmware PRIVATE ${OPEN_INCLUDES})
target_compile_definitions(firmware PRIVATE ${GD32H77X_DEVICE_DEFINE} USE_STDPERIPH_DRIVER HXTAL_VALUE=25000000U)
if(GD32H77X_PMU_DEFINE)
    target_compile_definitions(firmware PRIVATE ${GD32H77X_PMU_DEFINE})
endif()
target_compile_options(firmware PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
separate_arguments(OPEN_LIBRARIES NATIVE_COMMAND "${PLATFORM_NEED_LIBS}")
foreach(library IN LISTS OPEN_LIBRARIES)
    # Absolute archives are also Ninja dependencies: an application change must relink.
    list(APPEND OPEN_LIBRARY_FILES "${OPEN_LIBS_DIR}/lib${library}.a")
endforeach()
target_link_libraries(firmware PRIVATE -Wl,--start-group ${OPEN_LIBRARY_FILES} m c gcc -Wl,--end-group)
target_link_options(firmware PRIVATE "-T${LINKER}" "-L${GD32H77X_GENERATED_DIR}" -Wl,--gc-sections
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/firmware.map" --specs=nosys.specs)
set_target_properties(firmware PROPERTIES SUFFIX .elf LINK_DEPENDS
    "${LINKER};${LINKER_TEMPLATE};${GD32H77X_MEMORY_INCLUDE};${GD32H77X_LAYOUT_HEADER};${GD32H77X_LAYOUT_SOURCE};${GD32H77X_RESOLVED_LAYOUT}")
add_custom_target(gd32h77x_layout DEPENDS
    "${GD32H77X_MEMORY_INCLUDE}" "${GD32H77X_LAYOUT_HEADER}" "${GD32H77X_LAYOUT_SOURCE}" "${GD32H77X_RESOLVED_LAYOUT}")
add_dependencies(firmware gd32h77x_layout)
if(CONFIG_GD32H77X_ECNVM_CODE STREQUAL "y")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ecnvm_code.bin"
        COMMAND "${CMAKE_OBJCOPY}" -O binary --gap-fill 0xff
                --only-section=.ecnvm_text "$<TARGET_FILE:firmware>"
                "${CMAKE_CURRENT_BINARY_DIR}/ecnvm_code.bin"
        DEPENDS firmware
        COMMENT "Creating separate GD32H77X ECNVM executable payload"
        VERBATIM)
    add_custom_target(ecnvm_code_image ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ecnvm_code.bin")
    set(GD32H77X_CNVM_OBJCOPY_ARGS --remove-section=.ecnvm_text)
else()
    set(GD32H77X_CNVM_OBJCOPY_ARGS)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/firmware.bin"
    COMMAND "${CMAKE_OBJCOPY}" -O binary --gap-fill 0xff ${GD32H77X_CNVM_OBJCOPY_ARGS} "$<TARGET_FILE:firmware>"
            "${CMAKE_CURRENT_BINARY_DIR}/firmware.bin"
    DEPENDS firmware
    COMMENT "Creating complete GD32H77X flash image"
    VERBATIM
)
add_custom_target(firmware_image ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/firmware.bin")
add_custom_command(TARGET firmware POST_BUILD
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/firmware.map"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:firmware>"
    COMMAND "${CMAKE_COMMAND}" -E echo "Firmware linked with selected GD32H77X flash layout"
    VERBATIM
)
