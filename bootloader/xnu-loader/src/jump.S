#if defined(__aarch64__)

.text
.globl jump_to_xnu
.type jump_to_xnu, @function

jump_to_xnu:
    /* AAPCS64: x0=entry, x1=boot_args_phys, x2=stack_top.
     * Stashed in callee-saved registers because the cache flush below uses
     * x0-x11/x16/x17 as scratch. Nothing needs preserving for the kernel
     * except x0, which is set immediately before the branch. */
    mov x19, x0     /* entry (branch target) */
    mov x20, x1     /* boot_args_phys */
    mov x21, x2     /* stack_top */

    msr daifset, #0xf

    bl  dcache_clean_inval_all
    ic  iallu
    dsb nsh
    isb

    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne 1f

    /* RW=1 only. API/APK (bits 41/40) stay clear so PAuth is disabled below
     * EL2 and the instructions reach sleh.c's emulation instead of running
     * natively against keys nothing else agrees on. */
    mov x0, #(1 << 31)
    msr hcr_el2, x0

    /* CNTVOFF_EL2 is UNKNOWN out of reset and neither TF-A nor U-Boot clears
     * it, so leave it alone and EL1's virtual counter reads pure garbage. */
    msr cntvoff_el2, xzr
    mrs x0, cnthctl_el2
    orr x0, x0, #3         /* EL1PCTEN | EL1PCEN */
    msr cnthctl_el2, x0
    isb

    mrs x0, sctlr_el2
    bic x0, x0, #1         /* M  */
    bic x0, x0, #(1 << 2)  /* C  */
    bic x0, x0, #(1 << 12) /* I  */
    msr sctlr_el2, x0
    isb

    msr sctlr_el1, xzr
    isb

    mov x0, #0x3c5      /* SPSR: EL1h, all interrupts masked (DAIF=1111) */
    msr spsr_el2, x0
    msr elr_el2, x19
    mov sp, x21
    mov x0, x20         /* boot_args_phys */
    eret

1:
    mrs x1, sctlr_el1
    bic x1, x1, #1        /* M  */
    bic x1, x1, #(1 << 2) /* C  */
    bic x1, x1, #(1 << 12) /* I */
    msr sctlr_el1, x1
    isb

    mov sp, x21
    mov x0, x20     /* boot_args_phys */
    br x19

.type dcache_clean_inval_all, @function
dcache_clean_inval_all:
    dsb     sy
    mrs     x0, clidr_el1
    and     w3, w0, #0x07000000     /* level of coherency, bits [26:24]   */
    lsr     w3, w3, #23             /* ... as 2 * LoC                     */
    cbz     w3, 5f                  /* nothing to do                      */
    mov     w10, #0                 /* w10 = 2 * current level            */
    mov     w8, #1
1:
    add     w2, w10, w10, lsr #1    /* w2 = 3 * level, CLIDR field offset */
    lsr     w1, w0, w2
    and     w1, w1, #7              /* cache type at this level           */
    cmp     w1, #2
    b.lt    4f                      /* no data or unified cache here      */
    msr     csselr_el1, x10         /* select this level                  */
    isb
    mrs     x1, ccsidr_el1
    and     w2, w1, #7              /* log2(line length) - 4              */
    add     w2, w2, #4
    ubfx    w4, w1, #3, #10         /* max way number                     */
    clz     w5, w4                  /* bit position of the way field      */
    lsl     w9, w4, w5
    lsl     w16, w8, w5
2:
    ubfx    w7, w1, #13, #15        /* max set number                     */
    lsl     w7, w7, w2
    lsl     w17, w8, w2
3:
    orr     w11, w10, w9            /* level | way                        */
    orr     w11, w11, w7            /* ... | set                          */
    dc      cisw, x11               /* clean and invalidate by set/way    */
    subs    w7, w7, w17
    b.ge    3b
    subs    x9, x9, x16
    b.ge    2b
4:
    add     w10, w10, #2            /* next level                         */
    cmp     w3, w10
    dsb     sy
    b.gt    1b
5:
    mov     w10, #0                 /* restore CSSELR_EL1                 */
    msr     csselr_el1, x10
    dsb     sy
    isb
    ret

.section .note.GNU-stack,"",%progbits

#elif defined(__i386__)

/*
 * IA32 firmware handoff.
 */
.text
.code32
.globl pd_call_on_stack
.type pd_call_on_stack, @function

/*
 * pd_call_on_stack(void *new_sp, void (*fn)(void *), void *arg)
 * cdecl: 4(%esp)=new_sp, 8(%esp)=fn, 12(%esp)=arg.
 */
pd_call_on_stack:
    mov 4(%esp), %eax      /* new_sp */
    mov 8(%esp), %edx      /* fn */
    mov 12(%esp), %ecx     /* arg */
    mov %eax, %esp
    and $~0xF, %esp        /* keep the ABI's 16-byte alignment at the call */
    xor %ebp, %ebp         /* terminate frame-pointer chains on the new stack */
    sub $12, %esp          /* so esp+4 lands 16-byte aligned after the push */
    push %ecx              /* arg becomes fn's first parameter */
    call *%edx
1:  hlt                    /* fn is noreturn; trap if it ever comes back */
    jmp 1b

.globl jump_to_xnu
.type jump_to_xnu, @function

/*
 * jump_to_xnu(void *entry, UINT64 boot_args_phys, void *stack_top)
 * cdecl: 4(%esp)=entry, 8(%esp)=boot_args_phys (64-bit, low half at 8),
 *        16(%esp)=stack_top.
 */
jump_to_xnu:
    cli

    mov 4(%esp), %ebx      /* entry */
    mov 8(%esp), %ebp      /* boot_args_phys, low 32 bits - the block is
                            * allocated below 4GB, which 32-bit firmware
                            * cannot exceed anyway */
    mov 16(%esp), %esp     /* stack_top */
    and $~0xF, %esp

    /* Build the GDT *and* its GDTR on the stack. The 64-bit path can keep the
     * table in .data and reach it with a RIP-relative lea; i386 has no such
     * addressing, and naming the symbol emits an absolute relocation, which in
     * a shared object means DT_TEXTREL and the link is --fatal-warnings.
     * Three flat descriptors: null, 32-bit code, 32-bit data. */
    sub $24, %esp
    movl $0, 0(%esp)
    movl $0, 4(%esp)
    movl $0x0000ffff, 8(%esp)    /* code: limit 0xffff, base 0 */
    movl $0x00cf9a00, 12(%esp)   /* G=1 D=1 P=1 DPL=0, type=code r/x */
    movl $0x0000ffff, 16(%esp)   /* data: limit 0xffff, base 0 */
    movl $0x00cf9200, 20(%esp)   /* G=1 B=1 P=1 DPL=0, type=data r/w */
    mov %esp, %ecx               /* &gdt */

    sub $8, %esp
    movw $23, 0(%esp)            /* limit = 3*8 - 1 */
    movl %ecx, 2(%esp)
    lgdt 0(%esp)
    add $8, %esp                 /* esp now points at the GDT, which must stay
                                  * live until the segment loads below - so
                                  * nothing may push between here and there */

    /* Reload CS from our own GDT. A direct ljmp needs an absolute target
     * address, which is a relocation in .text - DT_TEXTREL again. Compute the
     * destination PC-relatively instead and far-return to it: the label
     * difference is within .text, so it assembles to a constant. */
    call 1f
1:  pop %eax                 /* eax = address of label 1 */
    add $(2f - 1b), %eax     /* eax = address of label 2 */
    push $0x08               /* CS */
    push %eax                /* EIP - lret pops EIP first, then CS */
    lret
2:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* IA32 firmware may run with paging enabled and identity-mapped; XNU
     * wants it off. */
    mov %cr0, %eax
    and $0x7fffffff, %eax
    mov %eax, %cr0

    jmp 3f                 /* pipeline flush */
3:
    mov %ebp, %eax         /* boot_args to the kernel */
    jmp *%ebx

.section .note.GNU-stack,"",@progbits

#else

.text
.code64
.globl pd_call_on_stack
.type pd_call_on_stack, @function

/*
 * pd_call_on_stack(void *new_sp, void (*fn)(void *), void *arg)
 *
 * Move to a caller-supplied stack and call fn there
 * SysV: rdi = new_sp, rsi = fn, rdx = arg.
 */
pd_call_on_stack:
    mov %rdi, %rsp
    and $~0xF, %rsp        /* ABI wants rsp 16-byte aligned at the call */
    xor %rbp, %rbp         /* terminate frame-pointer chains on the new stack */
    mov %rdx, %rdi         /* arg becomes fn's first parameter */
    call *%rsi
1:  hlt                    /* fn is noreturn; trap if it ever comes back */
    jmp 1b

.globl jump_to_xnu
.type jump_to_xnu, @function

jump_to_xnu:
    cli

    /* set stack */
    mov %rdx, %rsp
    and $~0xF, %rsp
    xor %rbp, %rbp

    /*
     * Build the 10-byte GDTR on the stack at runtime.
     * This avoids any absolute relocation against .data:
     *   [0..1]  limit  = gdt_end - gdt - 1
     *   [2..9]  base   = &gdt  (filled via RIP-relative lea)
     */
    sub $16, %rsp /* keep 16-byte alignment */
    movw $(gdt_end - gdt - 1), 0(%rsp)
    lea gdt(%rip), %rax
    mov %rax, 2(%rsp)
    lgdt 0(%rsp)
    add $16, %rsp

    /* stash entry + boot_args in callee-saved regs before lretq */
    mov %rdi, %rbx /* entry_phys */
    mov %rsi, %rbp /* boot_args */

    /* load 32-bit flat data segments in 64-bit mode so the descriptor
     * cache picks up base=0 from the GDT before the far jump.
     * In 64-bit mode this does not update GS_BASE MSR, that MSR is
     * cleared separately below so OVMF's value doesn't survive. */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* clear GS_BASE MSR, OVMF leaves a non-zero value */
    xor %eax, %eax
    xor %edx, %edx
    mov $0xC0000101, %ecx
    wrmsr
    mov $0xC0000102, %ecx
    wrmsr

    /* far jump into 32-bit compat CS */
    pushq $0x08
    lea compat32(%rip), %rax
    pushq %rax
    lretq


.code32
compat32:
    /* set data segments now that we're in 32-bit mode (base comes from GDT) */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* disable paging, matches Apple boot.efi trampoline */
    mov %cr0, %eax
    and $0x7fffffff, %eax
    mov %eax, %cr0

    /* clear EFER.LME, exit long mode entirely */
    mov $0xC0000080, %ecx
    rdmsr
    and $0xfffffeff, %eax
    wrmsr

    /* pipeline flush */
    jmp 1f
1:
    mov %ebp, %eax
    jmp *%ebx

.section .data
.align 8

gdt:
    .quad 0 /* null descriptor  */

    /* 0x08, 32-bit code */
    .word 0xffff, 0
    .byte 0, 0x9a, 0xcf, 0

    /* 0x10, 32-bit data */
    .word 0xffff, 0
    .byte 0, 0x92, 0xcf, 0

gdt_end:

.section .note.GNU-stack,"",@progbits

#endif /* __aarch64__ */
