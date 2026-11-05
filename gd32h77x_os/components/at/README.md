# AT component

The AT component is split by function rather than by module or consumer:

```text
at/
├── at_transport.*          transport contract
├── at_uart_transport.*     current UART byte channel
├── at_client.*             AT transaction client
├── at_parser.*             response/URC parser
├── at_event_queue.*        bounded event queue
├── at_session.*            transport-independent session
├── at_device.*             device fanout and reset hooks
├── at_module_reset.*       soft-first reset policy
├── at_profile.*            capability/profile policy
├── at_commands.h           isolated wire strings and formats
└── at_socket.*             generic AT socket semantics
```

Dependency direction:

```text
TuyaOpen TKL/TAL -> tuyaos_adapter -> at_socket
```

The AT component must not include or reference TuyaOpen/TKL/TAL headers, types,
error codes or functions. `at_socket` owns platform-neutral socket semantics;
`tuyaos/tuyaos_adapter/src/system/tkl_network.c` translates those semantics into
TKL return values and Tuya types.

`at_transport.h` is the only transport interface used by the session. UART is
the current implementation; SPI can be added as another transport without
changing the client, parser, session or socket layers. Code, files and APIs use
functional names such as `client`, `parser`, `socket`, `session` and
`uart_transport`; they do not identify a module, board or integration consumer.
