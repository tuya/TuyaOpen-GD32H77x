add_library(gd32h77x_drivers STATIC
    gd32h77x_cnvm.c
    gd32h77x_ecnvm.c
    gd32h77x_nor.c
    gd32h77x_flash_route.c
    gd32h77x_entropy.c
)
target_include_directories(gd32h77x_drivers PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gd32h77x_drivers PUBLIC gd32h77x_gd32_sdk gd32h77x_freertos)
