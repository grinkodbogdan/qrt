/* link.ld - the arm64 Image: linked at 0 as a position-independent executable; boot.S
 * applies the relocations for wherever the boot loader put it. */
ENTRY(_start)
SECTIONS {
    . = 0;
    /* Linux's code (Argon) in one range: the timer interrupt runs Linux's pending
     * interrupts only on top of Linux's own code (linux.c: argon_irq_tail) */
    .text : { KEEP(*(.text.head)) __lkl_text_start = .; *lkl.o(.text .text.*) __lkl_text_end = .; *(.text .text.*) }
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
    .notes : { *(.notes .note .note.*) }
    . = ALIGN(4096);
    __bss_start = .;
    .bss : { *(.bss .bss.* COMMON) }
    . = ALIGN(16);
    . += 0x40000;                       /* the boot stack: 256 KB */
    _stack_top = .;
    . = ALIGN(4096);
    __bss_end = .;
    _image_size = ABSOLUTE(__bss_end - _start);   /* a size, not an address: not relocated */
    /DISCARD/ : { *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.interp) *(.eh_frame*) *(.comment) }
}
