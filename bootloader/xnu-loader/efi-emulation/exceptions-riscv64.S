// s-mode trap vector for the risc-v image entry: saves x1..x31 and reports
// scause, sepc and stval through efiemu_riscv64_exception, which never returns

.text
.balign 4
.globl efiemu_riscv64_trap
efiemu_riscv64_trap:
  addi sp, sp, -256
  sd x1, 8(sp)
  sd x3, 24(sp)
  sd x4, 32(sp)
  sd x5, 40(sp)
  sd x6, 48(sp)
  sd x7, 56(sp)
  sd x8, 64(sp)
  sd x9, 72(sp)
  sd x10, 80(sp)
  sd x11, 88(sp)
  sd x12, 96(sp)
  sd x13, 104(sp)
  sd x14, 112(sp)
  sd x15, 120(sp)
  sd x16, 128(sp)
  sd x17, 136(sp)
  sd x18, 144(sp)
  sd x19, 152(sp)
  sd x20, 160(sp)
  sd x21, 168(sp)
  sd x22, 176(sp)
  sd x23, 184(sp)
  sd x24, 192(sp)
  sd x25, 200(sp)
  sd x26, 208(sp)
  sd x27, 216(sp)
  sd x28, 224(sp)
  sd x29, 232(sp)
  sd x30, 240(sp)
  sd x31, 248(sp)
  sd zero, 0(sp)
  // the sp the trap interrupted
  addi t0, sp, 256
  sd t0, 16(sp)
  mv a0, sp
  call efiemu_riscv64_exception
1:
  wfi
  j 1b
