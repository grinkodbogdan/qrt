/* link.ld - the arm64 Image: linked at 0 as a position-independent executable; boot.S
 * applies the relocations for wherever the boot loader put it. */
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : { KEEP(*(.text.head)) *(.text .text.*) }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(16);
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .got : { *(.got .got.*) }
    .dynamic : { *(.dynamic) }
    . = ALIGN(16);
    .rela.dyn : { __rela_start = .; *(.rela .rela.*) __rela_end = .; }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    __bss_start = .;
    .bss : { *(.bss .bss.* COMMON) }
    . = ALIGN(16);
    . += 0x40000;                       /* the boot stack: 256 KB */
    _stack_top = .;
    . = ALIGN(4096);
    __bss_end = .;
    _image_size = .;
    /DISCARD/ : { *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.interp) *(.eh_frame*) *(.note*) *(.comment) }
}
