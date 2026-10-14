/*
 * Linux x86 boot protocol, 32-bit entry. This shit is EVIL
 */

#define KERNEL_LINUX_MAGIC 0x584e554c /* "LUNX" */

.section .text.linux_entry,"ax"
.code32
.global linux_entry32
linux_entry32:
  cli
  cld
  mov %esi, %ebx

  /* ebp = where __kernel_start actually is. */
  call 1f
1:
  pop %ebp
  sub $1b, %ebp
  add $__kernel_start, %ebp

  mov $__kernel_start, %edi
  cmp %edi, %ebp
  je 3f

  mov $__file_end, %ecx
  sub $__kernel_start, %ecx
  mov %ebp, %esi
  cmp %edi, %esi
  ja 2f
  /* Destination above the source: copy backwards in case they overlap. */
  lea -1(%esi, %ecx), %esi
  lea -1(%edi, %ecx), %edi
  std
  rep movsb
  cld
  jmp 3f
2:
  rep movsb
3:
  mov $KERNEL_LINUX_MAGIC, %edi
  mov %ebx, %esi
  mov $kernel_boot32, %eax
  jmp *%eax

/*
 * 64-bit entry (XLF_KERNEL_64) at +0x200, where hypervisor direct boot (WSL2)
 * enters in long mode with rsi = boot_params and only the image identity-mapped.
 * Drop to 32-bit protected mode with paging off and reuse the 32-bit path.
 */
  .org 0x200
.code64
.global linux_entry64
linux_entry64:
  cli
  cld
  /* The 64-bit protocol provides no stack; use one inside the mapped image. */
  lea linux64_stack_top(%rip), %rsp
  lea linux64_gdt(%rip), %rax
  mov %rax, linux64_gdt_ptr+2(%rip)
  lgdt linux64_gdt_ptr(%rip)
  lea linux64_compat(%rip), %rax
  pushq $0x10
  push %rax
  lretq
.code32
linux64_compat:
  mov $0x18, %ax
  mov %ax, %ds
  mov %ax, %es
  mov %ax, %ss
  mov %cr0, %eax
  and $0x7fffffff, %eax              /* paging off: leaves long mode */
  mov %eax, %cr0
  mov $0xc0000080, %ecx              /* EFER.LME off */
  rdmsr
  and $~0x100, %eax
  wrmsr
  jmp linux_entry32

  .p2align 3
linux64_gdt:
  .quad 0
  .quad 0
  .quad 0x00cf9a000000ffff           /* 0x10: 32-bit code */
  .quad 0x00cf92000000ffff           /* 0x18: data */
linux64_gdt_ptr:
  .word 4 * 8 - 1
  .quad 0
  .p2align 3
  .fill 64, 8, 0
linux64_stack_top:

.section .note.GNU-stack,"",@progbits
