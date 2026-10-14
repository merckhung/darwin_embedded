// the riscv64 kernel's entry contract: s-mode, satp = 0, sie = 0 and sstatus.SIE clear,
// a0 = physical boot_args, a1 = boot hart id, the image already visible to instruction fetch

.text
.balign 4
.globl riscv64_jump_to_xnu
// a0 = physical _start, a1 = physical boot_args, a2 = hart id
riscv64_jump_to_xnu:
  csrw sie, zero
  csrci sstatus, 2
  csrw sip, zero
  csrw satp, zero
  sfence.vma
  fence rw, rw
  fence.i
  mv t0, a0
  mv a0, a1
  mv a1, a2
  jr t0
