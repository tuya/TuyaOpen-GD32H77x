/* Device regions come from the selected resolved flash layout. */
INCLUDE gd32h77x_memory.ldinc
ENTRY(Reset_Handler)
_sp = ORIGIN(AXISRAM) + LENGTH(AXISRAM);
_stack_bottom = _sp - 8K;
_ram_start = ORIGIN(AXISRAM);
_ram_end = ORIGIN(AXISRAM) + LENGTH(AXISRAM);
__vector_base = ORIGIN(CNVM);
SECTIONS
{
    .boot_table : { KEEP(*(.boot_table)) } > BOOT
    .vectors : {
        KEEP(*(.vectors))
        __Vectors_End = .;
    } > CNVM
    __Vectors = __gVectors;
    __Vectors_Size = __Vectors_End - __gVectors;
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
    .ramfunc : {
        . = ALIGN(4);
        _sramfunc = .;
        KEEP(*(.ramfunc*))
        . = ALIGN(4);
        _eramfunc = .;
    } > AXISRAM AT> CNVM
    _siramfunc = LOADADDR(.ramfunc);
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(8); _sdata = .;
        *(.data*)
        . = ALIGN(8); _edata = .;
    } > AXISRAM AT> CNVM
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
ASSERT(SIZEOF(.boot_table) == 8, "boot table must contain SP and reset entry")
ASSERT(ADDR(.vectors) == __vector_base, "vector table differs from selected flash layout")
ASSERT(SIZEOF(.ramfunc) > 0, "CNVM runtime command routine must be linked into AXI SRAM")
ASSERT(ADDR(.ramfunc) >= ORIGIN(AXISRAM) && ADDR(.ramfunc) + SIZEOF(.ramfunc) <= ORIGIN(AXISRAM) + LENGTH(AXISRAM), "RAM functions must execute from AXI SRAM")
ASSERT(LOADADDR(.ramfunc) >= ORIGIN(CNVM) && LOADADDR(.ramfunc) + SIZEOF(.ramfunc) <= ORIGIN(CNVM) + LENGTH(CNVM), "RAM function load image must fit CNVM")
ASSERT(_heap_end <= _stack_bottom, "AXI SRAM stack/heap overlap")
