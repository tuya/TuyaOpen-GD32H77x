ENTRY(Reset_Handler)

MEMORY
{
    CNVM   (rx)  : ORIGIN = 0x08000000, LENGTH = 2M
    ECNVM  (rx)  : ORIGIN = 0x08200000, LENGTH = 64K
    AXISRAM (rwx): ORIGIN = 0x24000000, LENGTH = 768K
}

_sp = ORIGIN(AXISRAM) + LENGTH(AXISRAM);
_stack_bottom = _sp - 8K;
_ram_start = ORIGIN(AXISRAM);
_ram_end = ORIGIN(AXISRAM) + LENGTH(AXISRAM);
__vector_base = ORIGIN(CNVM);

SECTIONS
{
    .vectors : { KEEP(*(.vectors)) __Vectors_End = .; } > CNVM
    __Vectors = ORIGIN(CNVM);
    __Vectors_Size = __Vectors_End - __Vectors;

    .text : {
        *(.text*) *(.rodata*) *(.glue_7*) *(.eh_frame*)
        KEEP(*(.init)) KEEP(*(.fini))
    } > CNVM
    .ARM.extab : { *(.ARM.extab*) } > CNVM
    .ARM.exidx : {
        __exidx_start = .; *(.ARM.exidx*) __exidx_end = .;
    } > CNVM
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > CNVM
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > CNVM
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > CNVM

    .ecnvm_text ORIGIN(ECNVM) : {
        __ecnvm_text_start = .;
        KEEP(*(.ecnvm_text))
        . = ALIGN(4);
        __ecnvm_text_end = .;
    } > ECNVM
    .ecnvm_rodata : {
        __ecnvm_rodata_start = .;
        KEEP(*(.ecnvm_rodata))
        . = ALIGN(4);
        __ecnvm_rodata_end = .;
    } > ECNVM

    .ramfunc : {
        . = ALIGN(4); _sramfunc = .;
        KEEP(*(.ramfunc*))
        . = ALIGN(4); _eramfunc = .;
    } > AXISRAM AT> CNVM
    _siramfunc = LOADADDR(.ramfunc);
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(8); _sdata = .;
        *(.data*)
        . = ALIGN(8); _edata = .;
    } > AXISRAM AT> CNVM
    __cnvm_payload_end = LOADADDR(.data) + SIZEOF(.data);
    .bss (NOLOAD) : {
        . = ALIGN(8); _sbss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8); _ebss = .;
    } > AXISRAM
    .heap (NOLOAD) : {
        . = ALIGN(8); _end = .;
        . += 16K; _heap_end = .;
    } > AXISRAM
    .ARM.attributes 0 : { *(.ARM.attributes) }
}

ASSERT(ADDR(.vectors) == ORIGIN(CNVM), "vendor vector table must begin at CNVM base")
ASSERT(Reset_Handler >= ORIGIN(CNVM) && Reset_Handler < ORIGIN(CNVM) + LENGTH(CNVM), "Reset_Handler must execute from CNVM")
ASSERT(main >= ORIGIN(CNVM) && main < ORIGIN(CNVM) + LENGTH(CNVM), "main must execute from CNVM")
ASSERT(__cnvm_payload_end <= 0x08010000, "CNVM experiment image must fit in first 64 KiB sector")
ASSERT(ADDR(.ecnvm_text) >= 0x08200000 && ADDR(.ecnvm_text) + SIZEOF(.ecnvm_text) <= 0x08210000, "ECNVM text must fit first 64 KiB")
ASSERT(ADDR(.ecnvm_rodata) >= 0x08200000 && ADDR(.ecnvm_rodata) + SIZEOF(.ecnvm_rodata) <= 0x08210000, "ECNVM rodata must fit first 64 KiB")
ASSERT(SIZEOF(.ecnvm_text) > 0 && SIZEOF(.ecnvm_rodata) > 0, "ECNVM test code and data must be present")
ASSERT(ADDR(.ramfunc) >= ORIGIN(AXISRAM) && ADDR(.ramfunc) + SIZEOF(.ramfunc) <= ORIGIN(AXISRAM) + LENGTH(AXISRAM), "RAM functions must fit AXI SRAM")
ASSERT(LOADADDR(.ramfunc) >= ORIGIN(CNVM) && LOADADDR(.ramfunc) + SIZEOF(.ramfunc) <= ORIGIN(CNVM) + LENGTH(CNVM), "RAM function load image must fit CNVM")
ASSERT(_heap_end <= _stack_bottom, "AXI SRAM stack/heap overlap")
