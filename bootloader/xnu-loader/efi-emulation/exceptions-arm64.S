/* AArch64 vector table for the Linux-image entry: every slot saves x0-x30
 * and reports ESR/ELR/FAR through efiemu_arm64_exception, which never returns. */
.macro slot n
  .balign 0x80
  sub sp, sp, #256
  stp x0, x1, [sp, #0]
  mov x0, #\n
  b efiemu_arm64_vector_common
.endm

.text
.balign 0x800
.globl efiemu_arm64_vectors
efiemu_arm64_vectors:
  slot 0
  slot 1
  slot 2
  slot 3
  slot 4
  slot 5
  slot 6
  slot 7
  slot 8
  slot 9
  slot 10
  slot 11
  slot 12
  slot 13
  slot 14
  slot 15

efiemu_arm64_vector_common:
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x19, [sp, #144]
  stp x20, x21, [sp, #160]
  stp x22, x23, [sp, #176]
  stp x24, x25, [sp, #192]
  stp x26, x27, [sp, #208]
  stp x28, x29, [sp, #224]
  str x30, [sp, #240]
  mov x1, sp
  bl efiemu_arm64_exception
1:
  wfi
  b 1b
