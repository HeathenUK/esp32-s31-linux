ENTRY(_start_qemu)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256M }
SECTIONS {
  .text : { *(.text.init) *(.text .text.*) } > RAM
  .s31hot : { KEEP(*(SORT_BY_NAME(s31hot_text*))) } > RAM
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > RAM
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE_HIDDEN(__init_array_end = .); } > RAM
  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } > RAM
  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); PROVIDE_HIDDEN(__preinit_array_end = .); } > RAM
  .eh_frame : { KEEP(*(.eh_frame)) } > RAM
  .gcc_except_table : { *(.gcc_except_table*) } > RAM
  .data : { __DATA_BEGIN__ = .; *(.data .data.*) *(.sdata .sdata.*) } > RAM
  PROVIDE(__global_pointer$ = __DATA_BEGIN__ + 0x800);
  .bss : { __bss_start = .; *(.sbss .sbss.* .bss .bss.* COMMON) ; _end = .; __BSS_END__ = .; } > RAM
  _edata = __bss_start;
  end = .;
  . = ORIGIN(RAM) + LENGTH(RAM) - 0x10000;
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
