add_library(gd32h77x_freertos STATIC
    tasks.c
    queue.c
    list.c
    timers.c
    event_groups.c
    stream_buffer.c
    portable/MemMang/heap_4.c
    portable/GCC/ARM_CM7/r0p1/port.c
    ${CMAKE_CURRENT_LIST_DIR}/../runtime/gd32h77x_task_mem.c
)

target_include_directories(gd32h77x_freertos PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/portable/GCC/ARM_CM7/r0p1
    ${CMAKE_CURRENT_LIST_DIR}/../runtime
)
