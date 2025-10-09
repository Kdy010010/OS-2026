ENTRY(kmain)
SECTIONS {
  . = 0x100000;
  .multiboot : { *(.multiboot) }
  .text :     { *(.text*) }
  .rodata :   { *(.rodata*) }
  .data :     { *(.data*) }
  .bss :      { *(.bss*) }
}
