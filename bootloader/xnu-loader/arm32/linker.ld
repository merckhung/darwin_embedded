OUTPUT_FORMAT(elf32-littlearm)
ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.head)) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(4);
  .rel.dyn : { __rel_start = .; *(.rel .rel.*) __rel_end = .; }
  .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
  .got : { *(.got .got.*) }
  .dynamic : { *(.dynamic) }
  .data : { *(.data .data.*) }
  . = ALIGN(4);
  __file_size = .;
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(16);
    . += 0x8000;
    __stack_top = .;
    __bss_end = .;
  }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) *(.interp) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
