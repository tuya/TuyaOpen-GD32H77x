set(GD32H77X_DRIVER_SRCS
    gd32h77x_cnvm.c
    gd32h77x_ecnvm.c
    gd32h77x_nor.c
    gd32h77x_ospi.c
    gd32h77x_psram_aps6404l.c
    gd32h77x_psram.c
    gd32h77x_psram_pool.c
    gd32h77x_psram_cache.c
    gd32h77x_psram_ring.c
    vendor/aps6404l.c
    vendor/systick.c
    gd32h77x_flash_route.c
    gd32h77x_entropy.c
    gd32h77x_dma.c
)

list(APPEND GD32H77X_DRIVER_SRCS
    gd32h77x_sdcard.c
    ../fatfs/ff.c
    ../fatfs/diskio.c
)

add_library(gd32h77x_drivers STATIC ${GD32H77X_DRIVER_SRCS})
target_include_directories(gd32h77x_drivers PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/vendor
    ${CMAKE_CURRENT_LIST_DIR}/../fatfs
)
target_link_libraries(gd32h77x_drivers PUBLIC gd32h77x_gd32_sdk gd32h77x_freertos)
