# Use the current TuyaOpen adapter contract rather than duplicate its headers.
list(APPEND PLATFORM_PUBINC
    "${PLATFORM_PATH}/gd32h77x_os/main"
    "${PLATFORM_PATH}/gd32h77x_os/wireless/vw553"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/flash"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/init/include"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/system"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/network"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/security"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/uart"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/wifi"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/bluetooth"
    "${PLATFORM_PATH}/tuyaos/tuyaos_adapter/include/utilities/include")


