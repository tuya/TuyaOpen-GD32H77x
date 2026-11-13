cmake_minimum_required(VERSION 3.20)
project(gd32h77x_firmware LANGUAGES C CXX ASM)

include("${BUILD_PARAM_PATH}/build_param.cmake")
if(NOT DEFINED GD32H77X_PROJECT_CONTEXT OR NOT EXISTS "${GD32H77X_PROJECT_CONTEXT}")
    message(FATAL_ERROR "Missing GD32H77X project context: ${GD32H77X_PROJECT_CONTEXT}")
endif()
include("${GD32H77X_PROJECT_CONTEXT}")
set(GD32H77X_PLATFORM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT DEFINED GD32H77X_PROJECT_DIR OR
        NOT EXISTS "${GD32H77X_PROJECT_DIR}/project.cmake")
    message(FATAL_ERROR "Invalid GD32H77X_PROJECT_DIR: ${GD32H77X_PROJECT_DIR}")
endif()
if((PLATFORM_CHIP STREQUAL "GD32H779VW" AND PLATFORM_BOARD STREQUAL "GD32H779V_START") OR
        (PLATFORM_CHIP STREQUAL "GD32H779IW" AND PLATFORM_BOARD STREQUAL "GD32H779I_DESK_BUDDY"))
    set(GD32H77X_DEVICE_DEFINE GD32H779XW)
else()
    message(FATAL_ERROR
        "Unsupported GD32H77X board/chip combination: ${PLATFORM_BOARD}/${PLATFORM_CHIP}")
endif()
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
set(LINKER_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/gd32h77x_os/components/startup/gd32h77x_linker.ld.in")
if(NOT EXISTS "${LINKER_TEMPLATE}")
    message(FATAL_ERROR "Missing GD32H77X linker template: ${LINKER_TEMPLATE}")
endif()
set(GD32H77X_DEFAULT_PARTITIONS "${GD32H77X_CHIPS_ROOT}/${GD32H77X_TARGET_CHIP}/partitions.csv")
set(GD32H77X_DEFAULT_DEVICES "${GD32H77X_CHIPS_ROOT}/${GD32H77X_TARGET_CHIP}/flash_devices.csv")
set(GD32H77X_BOARD_DIR "${OPEN_ROOT}/boards/GD32H77X/${PLATFORM_BOARD}")
set(GD32H77X_BOARD_PLATFORM_CONFIG_DIR "${GD32H77X_BOARD_DIR}/platform_config")
set(GD32H77X_BOARD_PARTITIONS "${GD32H77X_BOARD_PLATFORM_CONFIG_DIR}/partitions.csv")
set(GD32H77X_BOARD_DEVICES "${GD32H77X_BOARD_PLATFORM_CONFIG_DIR}/flash_devices.csv")
file(GLOB GD32H77X_BOARD_LAYOUT_STATE CONFIGURE_DEPENDS
    "${GD32H77X_BOARD_PARTITIONS}" "${GD32H77X_BOARD_DEVICES}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GD32H77X_LAYOUT_RESOLVER}" "${GD32H77X_DEFAULT_PARTITIONS}"
    "${GD32H77X_DEFAULT_DEVICES}"
    "${GD32H77X_GEOMETRIES}" "${GD32H77X_PLATFORM_CHIPS}" "${LINKER}"
    "${BUILD_PARAM_PATH}/build_param.json" "${GD32H77X_PROJECT_CONTEXT}")
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

# Platform-owned configuration is independent from TuyaOpen's Kconfig. The
# generic backend consumes Kconfig defaults, chip defaults and one explicitly
# selected project config. Board discovery and TuyaOpen staging happen in the
# caller before this project is configured.
set(GD32H77X_CONFIG_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/platform_config.py")
set(GD32H77X_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/platform_config")
set(GD32H77X_CONFIG_HEADER "${GD32H77X_CONFIG_DIR}/include/gd32h77x_config.h")
set(GD32H77X_CONFIG_CMAKE "${GD32H77X_CONFIG_DIR}/gd32h77x_config.cmake")
if(NOT DEFINED GD32H77X_PROJECT_CONFIG OR GD32H77X_PROJECT_CONFIG STREQUAL "")
    if(EXISTS "${GD32H77X_PROJECT_DIR}/project.config")
        set(GD32H77X_PROJECT_CONFIG "${GD32H77X_PROJECT_DIR}/project.config")
    else()
        unset(GD32H77X_PROJECT_CONFIG)
    endif()
endif()
if(DEFINED GD32H77X_PROJECT_CONFIG AND NOT EXISTS "${GD32H77X_PROJECT_CONFIG}")
    message(FATAL_ERROR "Missing GD32H77X project config: ${GD32H77X_PROJECT_CONFIG}")
endif()
file(GLOB GD32H77X_CONFIG_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gd32h77x_os/chips/*/*.config")
set(GD32H77X_PROJECT_CONFIG_ARGS)
if(DEFINED GD32H77X_PROJECT_CONFIG)
    list(APPEND GD32H77X_PROJECT_CONFIG_ARGS
        --project-config "${GD32H77X_PROJECT_CONFIG}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${GD32H77X_PROJECT_CONFIG}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GD32H77X_CONFIG_SCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig"
    ${GD32H77X_CONFIG_INPUTS})
execute_process(
    COMMAND "${GD32H77X_PYTHON}" "${GD32H77X_CONFIG_SCRIPT}"
            --platform-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --chip "${PLATFORM_CHIP}" --board "${PLATFORM_BOARD}"
            ${GD32H77X_PROJECT_CONFIG_ARGS}
            --out-dir "${GD32H77X_CONFIG_DIR}"
    RESULT_VARIABLE GD32H77X_CONFIG_RESULT
    OUTPUT_VARIABLE GD32H77X_CONFIG_OUTPUT
    ERROR_VARIABLE GD32H77X_CONFIG_ERROR)
if(NOT GD32H77X_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "GD32H77X platform configuration failed:
${GD32H77X_CONFIG_OUTPUT}${GD32H77X_CONFIG_ERROR}")
endif()
include("${GD32H77X_CONFIG_CMAKE}")

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
# The board partition table is the single source of truth for code placement.
# A declared ecnvm_code partition makes the resolver emit an executable ECNVM
# linker region; no separate platform feature switch is needed.
file(READ "${GD32H77X_RESOLVED_LAYOUT}" GD32H77X_RESOLVED_LAYOUT_JSON)
string(JSON GD32H77X_ECNVM_LAYOUT_TYPE
    ERROR_VARIABLE GD32H77X_ECNVM_LAYOUT_ERROR
    TYPE "${GD32H77X_RESOLVED_LAYOUT_JSON}" layout partitions ecnvm_code)
if(GD32H77X_ECNVM_LAYOUT_ERROR)
    set(GD32H77X_ECNVM_LAYOUT_ENABLED FALSE)
elseif(GD32H77X_ECNVM_LAYOUT_TYPE STREQUAL "OBJECT")
    set(GD32H77X_ECNVM_LAYOUT_ENABLED TRUE)
else()
    message(FATAL_ERROR "resolved layout ecnvm_code entry is not an object")
endif()
if(GD32H77X_ECNVM_LAYOUT_ENABLED)
    # Keep the boot table and vector table in CNVM. Move the application
    # instruction stream and read-only data into executable ECNVM.
    set(CNVM_TEXT_INPUTS "")
    set(ECNVM_OUTPUT_SECTIONS ".ecnvm_text : { *(.text*) *(.rodata*) } > ECNVM\n    ASSERT(SIZEOF(.ecnvm_text) > 0, \"ECNVM linker region declared but ECNVM text is empty\")")
else()
    set(CNVM_TEXT_INPUTS "*(.text*) *(.rodata*)")
    set(ECNVM_OUTPUT_SECTIONS "")
endif()
set(LINKER "${GD32H77X_GENERATED_DIR}/gd32h77x_linker.ld")
configure_file("${LINKER_TEMPLATE}" "${LINKER}" @ONLY)
set(GD32H77X_LAYOUT_DEPENDENCIES "${GD32H77X_LAYOUT_RESOLVER}"
    "${GD32H77X_DEFAULT_PARTITIONS}" "${GD32H77X_DEFAULT_DEVICES}"
    "${GD32H77X_GEOMETRIES}" "${GD32H77X_PLATFORM_CHIPS}" "${LINKER_TEMPLATE}"
    "${BUILD_PARAM_PATH}/build_param.json" "${GD32H77X_PROJECT_CONTEXT}")
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

set(GD32H77X_OS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gd32h77x_os")
set(GD32H77X_GD_ROOT "${GD32H77X_OS_ROOT}/gd32")
set(GD32H77X_COMPONENTS_ROOT "${GD32H77X_OS_ROOT}/components")

# Platform-generated headers are shared by every component. Device selection is
# a platform build input, not a per-component option.
include_directories(
    "${GD32H77X_CONFIG_DIR}/include"
    "${GD32H77X_GENERATED_DIR}/include")
add_compile_definitions(
    ${GD32H77X_DEVICE_DEFINE}
    USE_STDPERIPH_DRIVER
    HXTAL_VALUE=25000000U)
if(GD32H77X_PMU_DEFINE)
    add_compile_definitions(${GD32H77X_PMU_DEFINE})
endif()

# Vendor SDK support is a normal dependency directory. Components are discovered
# by their own CMakeLists; the components root intentionally has no build file.
add_subdirectory("${GD32H77X_GD_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/gd32")
file(GLOB GD32H77X_COMPONENT_CMAKELISTS CONFIGURE_DEPENDS
    "${GD32H77X_COMPONENTS_ROOT}/*/CMakeLists.txt")
list(SORT GD32H77X_COMPONENT_CMAKELISTS)
foreach(component_cmakelists IN LISTS GD32H77X_COMPONENT_CMAKELISTS)
    get_filename_component(component_dir "${component_cmakelists}" DIRECTORY)
    get_filename_component(component_name "${component_dir}" NAME)
    add_subdirectory("${component_dir}" "${CMAKE_CURRENT_BINARY_DIR}/components/${component_name}")
endforeach()

set(GD32H77X_PROJECT_SOURCES)
set(GD32H77X_PROJECT_INCLUDE_DIRS)
set(GD32H77X_PROJECT_LIBRARIES)
set(GD32H77X_PROJECT_LINK_TARGETS)
set(GD32H77X_PROJECT_REQUIRED_SYMBOLS)
set(GD32H77X_PROJECT_COMPILE_DEFINITIONS)
include("${GD32H77X_PROJECT_DIR}/project.cmake")
if(NOT GD32H77X_PROJECT_SOURCES)
    message(FATAL_ERROR "GD32H77X project did not declare GD32H77X_PROJECT_SOURCES: ${GD32H77X_PROJECT_DIR}")
endif()

add_executable(firmware
    ${GD32H77X_PROJECT_SOURCES}
    "${GD32H77X_LAYOUT_SOURCE}")
target_include_directories(firmware PRIVATE
    "${GD32H77X_CONFIG_DIR}/include"
    "${GD32H77X_GENERATED_DIR}/include")
if(GD32H77X_PROJECT_INCLUDE_DIRS)
    target_include_directories(firmware PRIVATE ${GD32H77X_PROJECT_INCLUDE_DIRS})
endif()
if(GD32H77X_PROJECT_COMPILE_DEFINITIONS)
    target_compile_definitions(firmware PRIVATE ${GD32H77X_PROJECT_COMPILE_DEFINITIONS})
endif()
target_compile_options(firmware PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_libraries(firmware PRIVATE
    ${GD32H77X_PROJECT_LINK_TARGETS}
    -Wl,--start-group
    gd32h77x_startup
    gd32h77x_runtime
    gd32h77x_console
    gd32h77x_drivers
    gd32h77x_at
    ${GD32H77X_PROJECT_LIBRARIES}
    m c gcc
    -Wl,--end-group)
target_link_options(firmware PRIVATE "-T${LINKER}" "-L${GD32H77X_GENERATED_DIR}" -Wl,--gc-sections
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/firmware.map" --specs=nosys.specs)
foreach(symbol IN LISTS GD32H77X_PROJECT_REQUIRED_SYMBOLS)
    target_link_options(firmware PRIVATE "-Wl,--require-defined=${symbol}")
endforeach()
set_target_properties(firmware PROPERTIES SUFFIX .elf LINK_DEPENDS
    "${LINKER};${LINKER_TEMPLATE};${GD32H77X_MEMORY_INCLUDE};${GD32H77X_LAYOUT_HEADER};${GD32H77X_LAYOUT_SOURCE};${GD32H77X_RESOLVED_LAYOUT};${GD32H77X_CONFIG_HEADER}")
add_custom_target(gd32h77x_layout DEPENDS
    "${GD32H77X_MEMORY_INCLUDE}" "${GD32H77X_LAYOUT_HEADER}" "${GD32H77X_LAYOUT_SOURCE}" "${GD32H77X_RESOLVED_LAYOUT}")
add_dependencies(firmware gd32h77x_layout)
if(GD32H77X_ECNVM_LAYOUT_ENABLED)
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
