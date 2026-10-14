#if defined(__arm64__)
.text
.globl _native_dyld_start
.p2align 2
_native_dyld_start:
    ldr x0, [sp]
    mov x1, sp
    and x2, x1, #-16
    mov sp, x2
    bl _native_dyld_entry
    brk #0
#else
.text
.globl _native_dyld_start
.p2align 4
_native_dyld_start:
    movq (%rsp), %rdi
    movq %rsp, %rsi
    andq $-16, %rsp
    callq _native_dyld_entry
    ud2
#endif
