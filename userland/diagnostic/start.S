#if defined(__aarch64__)
.text
.globl _start
.p2align 2
_start:
    // LC_MAIN receives argc and argv through the normal Darwin C ABI.
    b _user_main
#else
.text
.globl _start
.p2align 4
_start:
    // Preserve LC_MAIN's C arguments and return address.
    jmp _user_main
#endif
