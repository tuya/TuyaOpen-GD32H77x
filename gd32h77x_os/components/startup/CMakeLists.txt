add_library(gd32h77x_startup INTERFACE)
target_sources(gd32h77x_startup INTERFACE
    main.c
    startup_gd32h77d.S
    ${GD32H77X_GD_ROOT}/CMSIS/GD/GD32H77x_78x/Source/GCC/startup_gd32h77x_78x.S
)
target_include_directories(gd32h77x_startup INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gd32h77x_startup INTERFACE
    gd32h77x_gd32_sdk
    gd32h77x_runtime
    gd32h77x_drivers)
