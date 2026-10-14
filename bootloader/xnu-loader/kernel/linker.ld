OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS {
  /* x86 XNU is copied to physical 1 MiB just before the jump, so the loader
   * must live well clear of it and its boot_args (0x2800000). */
  . = 0x10000000;
  __kernel_start = .;
  /* The Linux payload starts at its entry; GRUB scans only the first 32 KiB
   * of the file for the Multiboot2 header. */
  .text.linux_entry : { KEEP(*(.text.linux_entry)) }
  .multiboot2 : { KEEP(*(.multiboot2)) }
  .text : { *(.text.entry) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  __file_end = .;
  .bss : { __bss_start = .; *(.bss .bss.* COMMON) __bss_end = .; }
  __kernel_end = .;
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
