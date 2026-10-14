#if defined(__aarch64__)
.text
.globl _dyld_start
.p2align 2
_dyld_start:
    mov x19, sp
    ldr x0, [sp]       // XNU prepends the main Mach-O header to argc/argv.
    mov x1, sp
    and x1, x1, #-16
    mov sp, x1
    bl _diagnostic_entry
    cbz x0, 1f
    mov x4, x0
    ldr w0, [x19, #8]  // argc
    add x1, x19, #16   // argv
    add x2, x1, x0, lsl #3
    add x2, x2, #8     // envp follows argv's terminating null
    mov x3, x2
0:  ldr x5, [x3], #8
    cbnz x5, 0b       // apple follows envp's terminating null
    blr x4            // LC_MAIN is a C function, not a raw-stack entry.
    b 2f
1:  mov x0, #78
2:  mov x16, #1
    svc #0x80
    b 2b
#else
.text
.globl _dyld_start
.p2align 4
_dyld_start:
    movq %rsp, %r12
    movq (%rsp), %rdi
    andq $-16, %rsp
    callq _diagnostic_entry
    testq %rax, %rax
    jz 1f
    movl 8(%r12), %edi
    leaq 16(%r12), %rsi
    leaq 8(%rsi,%rdi,8), %rdx
    movq %rdx, %rcx
0:  cmpq $0, (%rcx)
    leaq 8(%rcx), %rcx
    jne 0b
    callq *%rax        // Stack remains aligned for the Darwin C ABI.
    movl %eax, %edi
    jmp 2f
1:  movl $78, %edi
2:  movl $0x2000001, %eax
    syscall
    jmp 2b
#endif
