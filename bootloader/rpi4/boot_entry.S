.section .text.boot, "ax"
.global _start
_start:
    // Check CPU ID: only CPU 0 proceeds
    mrs x1, mpidr_el1
    and x1, x1, #0xff
    cbz x1, primary_cpu

secondary_hang:
    wfe
    b secondary_hang

primary_cpu:
    // Save DTB (passed in x0) to x20
    mov x20, x0
    
    // UART Init in EL2/EL3
    mov x0, #0x1030
    movk x0, #0xfe20, lsl #16
    mov w1, #0x0
    str w1, [x0]
    
    mov x0, #0x102c
    movk x0, #0xfe20, lsl #16
    mov w1, #0x70
    str w1, [x0]
    
    mov x0, #0x1030
    movk x0, #0xfe20, lsl #16
    mov w1, #0x301
    str w1, [x0]

    // Print '!' to UART
    mov x0, #0x1000
    movk x0, #0xfe20, lsl #16
    mov w1, #0x21 // '!'
    str w1, [x0]

    // Check Current EL
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.eq drop_to_el1
    cmp x0, #3
    b.eq drop_to_el1_from_el3
    b el1_entry

drop_to_el1_from_el3:
    // The non-secure kernel needs Group 1 PPIs/SPIs. Assign groups while EL3
    // can still access GICD_IGROUPR; these writes are RAZ/WI from non-secure EL1.
    mov x2, #0x1000
    movk x2, #0xff84, lsl #16
    ldr w3, [x2, #4]
    and w3, w3, #0x1f
    add w3, w3, #1
    add x2, x2, #0x80
    mov w1, #-1
1:
    str w1, [x2], #4
    subs w3, w3, #1
    b.ne 1b
    dsb sy
    // Drop to EL2 first
    ldr x0, =(1 << 10) | (1 << 0) // SCR_EL3: RW=1, NS=1
    msr scr_el3, x0
    ldr x0, =0x3c9 // SPSR_EL3: Return to EL2h
    msr spsr_el3, x0
    adr x0, drop_to_el1
    msr elr_el3, x0
    eret

drop_to_el1:
    // Setup HCR_EL2: RW=1 (EL1 is AArch64)
    ldr x0, =(1 << 31)
    msr hcr_el2, x0
    // Permit EL1 access to the architectural counter and physical timer.
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    
    // Set SPSR_EL2 to return to EL1h with interrupts masked (DAIF=1111)
    ldr x0, =0x3c5
    msr spsr_el2, x0
    
    // Set ELR_EL2 to the continuation address
    adr x0, el1_entry
    msr elr_el2, x0
    eret

el1_entry:
    // We are now in EL1
    // Enable NEON/FP (CPACR_EL1.FPEN = 11)
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    // Disable MMU, caches and alignment traps for C's packed tree copies.
    mrs x0, sctlr_el1
    ldr x1, =~(1 | (1<<1) | (1<<2) | (1<<12))
    and x0, x0, x1
    msr sctlr_el1, x0
    isb

    // Pass DTB to C code
    mov x0, x20
    
    // Setup a small stack
    adr x1, stack_top
    mov sp, x1
    
    // Initialize TPIDR_EL1 to a safe buffer (e.g., end of stack)
    adr x1, tpidr_buffer
    msr tpidr_el1, x1
    
    bl setup_and_boot
    
hang:
    wfe
    b hang

    .bss
    .align 4
    .space 4096
stack_top:
tpidr_buffer:
    .space 4096
