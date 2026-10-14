OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  /* position independent, linked at 0 and relocated by the entry stub */
  . = 0;
  __kernel_start = .;
  .text.linux_entry : { KEEP(*(.text.linux_entry)) }
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
  . = ALIGN(8);
  .rela.dyn : { __rela_start = .; *(.rela .rela.*) __rela_end = .; }
  .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
  .got : { *(.got .got.*) }
  .dynamic : { *(.dynamic) }
  .data : { *(.data .data.*) *(.sdata .sdata.*) }
  . = ALIGN(4096);
  __file_end = .;
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.* COMMON) *(.sbss .sbss.*)
    . = ALIGN(16);
    . += 0x20000;
    __stack_top = .;
    . = ALIGN(4096);
    __bss_end = .;
  }
  __kernel_end = .;
  __image_size = __kernel_end - __kernel_start;
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) *(.interp) *(.riscv.attributes) }
}
