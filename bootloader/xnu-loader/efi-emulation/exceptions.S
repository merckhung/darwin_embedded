/* Stubs for CPU exceptions 0-31: push a uniform frame, call the C reporter. */

.macro EXCEPTION_NOERR n
.global efiemu_exception_\n
efiemu_exception_\n:
  push $0
  push $\n
  jmp exception_common
.endm

.macro EXCEPTION_ERR n
.global efiemu_exception_\n
efiemu_exception_\n:
  push $\n
  jmp exception_common
.endm

.section .text
EXCEPTION_NOERR 0
EXCEPTION_NOERR 1
EXCEPTION_NOERR 2
EXCEPTION_NOERR 3
EXCEPTION_NOERR 4
EXCEPTION_NOERR 5
EXCEPTION_NOERR 6
EXCEPTION_NOERR 7
EXCEPTION_ERR 8
EXCEPTION_NOERR 9
EXCEPTION_ERR 10
EXCEPTION_ERR 11
EXCEPTION_ERR 12
EXCEPTION_ERR 13
EXCEPTION_ERR 14
EXCEPTION_NOERR 15
EXCEPTION_NOERR 16
EXCEPTION_ERR 17
EXCEPTION_NOERR 18
EXCEPTION_NOERR 19
EXCEPTION_NOERR 20
EXCEPTION_ERR 21
EXCEPTION_NOERR 22
EXCEPTION_NOERR 23
EXCEPTION_NOERR 24
EXCEPTION_NOERR 25
EXCEPTION_NOERR 26
EXCEPTION_NOERR 27
EXCEPTION_NOERR 28
EXCEPTION_NOERR 29
EXCEPTION_ERR 30
EXCEPTION_NOERR 31

exception_common:
  push %rax
  push %rcx
  push %rdx
  push %rbx
  push %rbp
  push %rsi
  push %rdi
  push %r8
  push %r9
  push %r10
  push %r11
  push %r12
  push %r13
  push %r14
  push %r15
  mov %rsp, %rdi
  and $-16, %rsp
  call efiemu_exception_report
1:
  cli
  hlt
  jmp 1b

.section .rodata
.global efiemu_exception_stubs
efiemu_exception_stubs:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  .quad efiemu_exception_\n
.endr

.section .note.GNU-stack,"",@progbits
