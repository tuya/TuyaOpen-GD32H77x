/* Independent reset path: the vendor vector table starts at CNVM address 0. */
.syntax unified
.cpu cortex-m7
.thumb
.section .text.gd32h77d_reset,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
Reset_Handler:
    cpsid i
    /* Initialize all AXI SRAM for ECC before stack, heap, or C data use. */
    ldr r0, =_ram_start
    ldr r1, =_ram_end
    movs r2, #0
1:
    str r2, [r0], #4
    cmp r0, r1
    blo 1b
    ldr r0, =_sp
    msr msp, r0
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
2:
    cmp r1, r2
    bhs 3f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 2b
3:
    ldr r0, =_siramfunc
    ldr r1, =_sramfunc
    ldr r2, =_eramfunc
4:
    cmp r1, r2
    bhs 5f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 4b
5:
    bl SystemInit
    bl __libc_init_array
    bl main
6:
    b 6b
.size Reset_Handler, .-Reset_Handler
