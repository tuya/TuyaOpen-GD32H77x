add_library(gd32h77x_console STATIC gd32h77x_console.c)
target_include_directories(gd32h77x_console PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../runtime)
target_link_libraries(gd32h77x_console PUBLIC
    gd32h77x_gd32_sdk
    gd32h77x_freertos)
