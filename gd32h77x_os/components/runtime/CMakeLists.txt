add_library(gd32h77x_runtime INTERFACE)
target_sources(gd32h77x_runtime INTERFACE
    gd32h77x_platform.c
    syscalls.c
    gd32h77x_native_hooks.c
)
target_include_directories(gd32h77x_runtime INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gd32h77x_runtime INTERFACE
    gd32h77x_freertos
    gd32h77x_drivers)
