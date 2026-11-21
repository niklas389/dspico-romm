// SPDX-License-Identifier: MIT
//
// Timer 0 IRQ entry for the sampling profiler (see prof.c).
//
// libnds' IntrMain switches to system mode and pushes lr_sys before calling
// the handler, so at entry [sp] holds the link register of the interrupted
// code. For leaf functions such as memcpy that is the caller's address.

    .syntax unified
    .arm
    .section .text.prof_irq_entry, "ax", %progbits
    .global prof_irq_entry
    .type   prof_irq_entry, %function
prof_irq_entry:
    ldr     r0, [sp]            // lr of the interrupted code
    ldr     r3, =prof_irq_c
    bx      r3
    .size   prof_irq_entry, . - prof_irq_entry
    .pool
