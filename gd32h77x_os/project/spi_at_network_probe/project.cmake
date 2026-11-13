set(GD32H77X_PROJECT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/network_probe.c")
set(GD32H77X_PROJECT_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND GD32H77X_PROJECT_LINK_TARGETS gd32h77x_console gd32h77x_at)

# Compile the production TKL network adapter for hardware API verification.
add_subdirectory("${GD32H77X_PLATFORM_ROOT}/tuyaos/tuyaos_adapter"
                 "${CMAKE_BINARY_DIR}/tkl_probe_headers")
list(APPEND GD32H77X_PROJECT_SOURCES
    "${GD32H77X_PLATFORM_ROOT}/tuyaos/tuyaos_adapter/src/system/tkl_network.c")
list(APPEND GD32H77X_PROJECT_INCLUDE_DIRS
    "${GD32H77X_PLATFORM_ROOT}/../../src/common/include")
list(APPEND GD32H77X_PROJECT_LINK_TARGETS gd32h77x_tuyaos_adapter_headers)
# Native projects do not run the SDK top-level Kconfig generator. Bridge only
# the existing generated platform symbols for the production adapter headers.
set(TKL_PROBE_CONFIG_DIR "${CMAKE_BINARY_DIR}/generated/tkl_probe")
file(MAKE_DIRECTORY "${TKL_PROBE_CONFIG_DIR}")
file(WRITE "${TKL_PROBE_CONFIG_DIR}/tuya_kconfig.h"
    "#pragma once\n#include \"gd32h77x_config.h\"\n#define OPERATING_SYSTEM SYSTEM_FREERTOS\n")
list(APPEND GD32H77X_PROJECT_INCLUDE_DIRS "${TKL_PROBE_CONFIG_DIR}")
