// risc-v linux image entry (documentation/arch/riscv/boot-image-header.rst)
// sbi enters in s-mode with a0 = hart id, a1 = fdt and satp = 0, the image is position independent

#define SSTATUS_SIE        (1 << 1)
#define SSTATUS_FS_INITIAL (1 << 13)
#define SBI_EXT_HSM        0x48534d
#define SBI_HSM_HART_STOP  1

.section .text.linux_entry, "ax"
.globl _start
_start:
  // code0 has to be one 4-byte jump
  .option push
  .option norvc
  j primary_entry
  .option pop
  .word 0                         // code1
  .dword 0x200000                 // text_offset, as linux rv64 asks for
  .dword __image_size             // image_size, bss and stack included
  .dword 0                        // flags, little endian
  .word 0x2                       // version 0.2
  .word 0                         // res1
  .dword 0                        // res2
  .ascii "RISCV\0\0\0"            // magic, deprecated
  .ascii "RSC\x05"                // magic2
  .word 0                         // res3, no pe header

primary_entry:
  csrw sie, zero
  csrci sstatus, SSTATUS_SIE
  // the first hart through boots, any other one is handed back to sbi
  lla t0, boot_lottery
  li t1, 1
  amoswap.w.aq t1, t1, (t0)
  bnez t1, park

  mv s0, a0
  mv s1, a1
  csrw satp, zero
  sfence.vma
  // the loader's c code may touch fp registers
  li t0, SSTATUS_FS_INITIAL
  csrs sstatus, t0
  fscsr zero
  lla sp, __stack_top

  lla t0, __bss_start
  lla t1, __bss_end
1:
  bgeu t0, t1, 2f
  sd zero, 0(t0)
  addi t0, t0, 8
  j 1b
2:
  // linked at 0, every R_RISCV_RELATIVE slot gets the load address added
  lla t0, _start
  lla t1, __rela_start
  lla t2, __rela_end
  li t6, 3
3:
  bgeu t1, t2, 4f
  ld t3, 0(t1)
  ld t4, 8(t1)
  ld t5, 16(t1)
  addi t1, t1, 24
  bne t4, t6, 3b
  add t5, t5, t0
  add t3, t3, t0
  sd t5, 0(t3)
  j 3b
4:
  fence.i
  mv a0, s0
  mv a1, s1
  call kernel_riscv64_main

park:
  // stopped through hsm the kernel can start this hart again later
  csrw sie, zero
  li a7, SBI_EXT_HSM
  li a6, SBI_HSM_HART_STOP
  ecall
5:
  wfi
  j 5b

.section .data
.balign 4
boot_lottery:
  .word 0
