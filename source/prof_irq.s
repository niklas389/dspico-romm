// SPDX-License-Identifier: MIT
//
// Timer 0 IRQ entry for the sampling profiler (see prof.c).
//
// The handler may run in IRQ mode, where lr is banked. Read the link register
// of the interrupted code directly from the system/user bank (shared by both
// modes): briefly switch to system mode with IRQs disabled. For leaf
// functions such as memcpy that is the caller's address.
// r1 = [sp] is passed too, in case the handler already runs in system mode
// (then lr_sys was pushed by IntrMain and is at [sp]).

    .syntax unified
    .arm
    .section .text.prof_irq_entry, "ax", %progbits
    .global prof_irq_entry
    .type   prof_irq_entry, %function
prof_irq_entry:
    mrs     r2, cpsr
    orr     r3, r2, #0x9F       // System mode (0x1F) + IRQ disabled (0x80)
    msr     cpsr_c, r3
    mov     r0, lr              // lr_usr/sys of the interrupted code
    msr     cpsr_c, r2
    and     r3, r2, #0x1F
    cmp     r3, #0x1F
    ldreq   r0, [sp]            // Already in system mode: use the pushed lr
    ldr     r3, =prof_irq_c
    bx      r3
    .size   prof_irq_entry, . - prof_irq_entry
    .pool
