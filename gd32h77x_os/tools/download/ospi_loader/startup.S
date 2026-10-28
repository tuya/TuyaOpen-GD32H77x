/* Copyright (c) 2026 Tuya Inc. SPDX-License-Identifier: Apache-2.0 */
.syntax unified
.cpu cortex-m7
.thumb
.section .text.startup,"ax",%progbits
.global _start
.type _start,%function
_start:
    cpsid i
    /* The loader is entered by a debugger: force privileged MSP execution. */
    movs r0, #0
    msr control, r0
    isb
    /* The host and CPU share SRAM through the debug AP, so run uncached. */
    ldr r0, =0xE000ED14
    ldr r1, [r0]
    bic r1, r1, #0x00030000
    str r1, [r0]
    dsb
    isb
    /* Initialize the ECC-protected AXI SRAM stack before the first C call. */
    ldr r0, =_stack_bottom
    ldr r1, =_stack_top
    movs r2, #0
1:
    cmp r0, r1
    bhs 2f
    str r2, [r0], #4
    b 1b
2:
    ldr r0, =_stack_top
    msr msp, r0
    bl ospi_loader_main
3:
    b 3b
.size _start, .-_start
