#define MB2_HEADER_MAGIC 0xe85250d6
#define MB2_BOOTLOADER_MAGIC 0x36d76289
#define MB2_ARCH_I386 0

.section .multiboot2,"a"
.align 8
mb2_header:
  .long MB2_HEADER_MAGIC
  .long MB2_ARCH_I386
  .long mb2_header_end - mb2_header
  .long -(MB2_HEADER_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))

  /* Framebuffer request (optional: GRUB may still boot us in text mode). */
  .align 8
  .word 5
  .word 1
  .long 20
  .long 1024
  .long 768
  .long 32

  .align 8
  .word 0
  .word 0
  .long 8
mb2_header_end:

.section .text.entry,"ax"
.code32
.global _start
_start:
  cli
  cld
  mov %eax, %edi
  mov %ebx, %esi

/* EDI = protocol magic, ESI = protocol info; checked in kernel_boot_main. */
.global kernel_boot32
kernel_boot32:
  cli
  cld
  /* Clear .bss first: the stack and page tables live there. */
  mov $__bss_start, %eax
  mov $__bss_end, %ecx
3:
  cmp %ecx, %eax
  jae 4f
  movb $0, (%eax)
  inc %eax
  jmp 3b
4:
  mov $boot_stack_top, %esp

  /* Identity-map the first 4 GiB with 2 MiB pages. */
  mov $boot_pdpt, %eax
  or $3, %eax
  mov %eax, boot_pml4

  xor %ecx, %ecx
5:
  mov %ecx, %eax
  shl $12, %eax
  add $boot_pd, %eax
  or $3, %eax
  mov %eax, boot_pdpt(, %ecx, 8)
  inc %ecx
  cmp $4, %ecx
  jb 5b

  xor %ecx, %ecx
6:
  mov %ecx, %eax
  shl $21, %eax
  or $0x83, %eax
  mov %eax, boot_pd(, %ecx, 8)
  inc %ecx
  cmp $(4 * 512), %ecx
  jb 6b

  mov %cr4, %eax
  or $(1 << 5), %eax
  mov %eax, %cr4

  mov $boot_pml4, %eax
  mov %eax, %cr3

  mov $0xc0000080, %ecx
  rdmsr
  or $(1 << 8), %eax
  wrmsr

  mov %cr0, %eax
  or $(1 << 31 | 1), %eax
  mov %eax, %cr0

  lgdt boot_gdt_pointer
  ljmp $0x08, $long_mode_entry

.code64
long_mode_entry:
  mov $0x10, %ax
  mov %ax, %ds
  mov %ax, %es
  mov %ax, %fs
  mov %ax, %gs
  mov %ax, %ss
  movabs $boot_stack_top, %rsp
  xor %ebp, %ebp
  /* SysV: rdi = protocol magic, rsi = physical address of its info. */
  mov %edi, %edi
  mov %esi, %esi
  call kernel_boot_main
7:
  cli
  hlt
  jmp 7b

.section .rodata
.align 8
boot_gdt:
  .quad 0
  .quad 0x00af9a000000ffff
  .quad 0x00cf92000000ffff
boot_gdt_end:
boot_gdt_pointer:
  .word boot_gdt_end - boot_gdt - 1
  .long boot_gdt

.section .bss
.align 4096
boot_pml4:
  .skip 4096
boot_pdpt:
  .skip 4096
boot_pd:
  .skip 4 * 4096
.align 16
  .skip 64 * 1024
boot_stack_top:

.section .note.GNU-stack,"",@progbits
