.section .text.entry,"ax"
.global legacy_entry
.type legacy_entry,@function
legacy_entry:
    cld
    mov %rcx, %r12
    lea __bss_start(%rip), %rax
    lea __bss_end(%rip), %rcx
1:  cmp %rcx, %rax
    jae 2f
    movb $0, (%rax)
    inc %rax
    jmp 1b
2:  and $-16, %rsp
    mov %r12, %rcx
    call legacy_firmware_main
3:  cli
    hlt
    jmp 3b

.section .note.GNU-stack,"",@progbits
