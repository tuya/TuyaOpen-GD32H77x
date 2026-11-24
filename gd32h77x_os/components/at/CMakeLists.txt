add_library(gd32h77x_at_core STATIC
    at_client.c
    at_parser.c
    at_event_queue.c
    at_session.c
    at_device.c
    at_module_reset.c
    at_profile.c
    at_socket.c
)
target_include_directories(gd32h77x_at_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gd32h77x_at_core PUBLIC gd32h77x_freertos gd32h77x_gd32_sdk)

if(CONFIG_ENABLE_GD32H77X_AT_TRANSPORT_SPI)
    target_compile_definitions(gd32h77x_at_core PRIVATE AT_TRANSPORT_SPI=1)
    add_library(gd32h77x_at_spi_transport INTERFACE)
    target_sources(gd32h77x_at_spi_transport INTERFACE
        at_transport.c
        at_spi_transport.c
        at_spi_protocol.c
    )
    target_include_directories(gd32h77x_at_spi_transport INTERFACE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_definitions(gd32h77x_at_spi_transport INTERFACE AT_TRANSPORT_SPI=1)
    if(CONFIG_ENABLE_GD32H77X_AT_SPI_CS_ONLY)
        target_compile_definitions(gd32h77x_at_spi_transport INTERFACE AT_SPI_CS_ONLY=1)
    endif()
    target_link_libraries(gd32h77x_at_spi_transport INTERFACE
        gd32h77x_at_core
        gd32h77x_drivers)
    set(GD32H77X_AT_TRANSPORT_TARGET gd32h77x_at_spi_transport)
else()
    add_library(gd32h77x_at_uart_transport INTERFACE)
    target_sources(gd32h77x_at_uart_transport INTERFACE
        at_transport.c
        at_uart_transport.c
    )
    target_include_directories(gd32h77x_at_uart_transport INTERFACE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_definitions(gd32h77x_at_uart_transport INTERFACE AT_TRANSPORT_UART=1)
    target_link_libraries(gd32h77x_at_uart_transport INTERFACE
        gd32h77x_at_core
        gd32h77x_drivers)
    set(GD32H77X_AT_TRANSPORT_TARGET gd32h77x_at_uart_transport)
endif()

add_library(gd32h77x_at INTERFACE)
target_link_libraries(gd32h77x_at INTERFACE
    gd32h77x_at_core
    ${GD32H77X_AT_TRANSPORT_TARGET})
