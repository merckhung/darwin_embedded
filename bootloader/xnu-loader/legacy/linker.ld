OUTPUT_FORMAT(elf64-x86-64)
ENTRY(legacy_entry)
SECTIONS {
  . = 0x20000;
  __kernel_start = .;
  .text : { *(.text.entry) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { __bss_start = .; *(.bss .bss.* COMMON) __bss_end = .; }
  __kernel_end = .;
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
