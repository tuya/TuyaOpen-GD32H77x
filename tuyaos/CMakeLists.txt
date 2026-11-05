add_subdirectory(tuyaos_adapter)

add_library(gd32h77x_at_stack INTERFACE)
target_link_libraries(gd32h77x_at_stack INTERFACE
    gd32h77x_at
    gd32h77x_tuyaos_adapter)
