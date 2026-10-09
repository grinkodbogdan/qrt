/*
 * boot.S - Tessera's entry on 64-bit ARM, as a Linux arm64 "Image" (Documentation/arch/
 * arm64/booting.rst), so the same file boots from QEMU (-kernel) and from an Android
 * boot loader (boot.img: Image.gz followed by the device tree).
 *
 * The boot loader enters at offset 0 with the MMU off, x0 = the device tree's physical
 * address, at EL2 or EL1.  The image is position-independent: it may be loaded anywhere
 * (aboot uses 0x80008000, QEMU the start of RAM + 2 MB), so the R_AARCH64_RELATIVE
 * relocations are applied here before any C runs.
 */
    .section .text.head, "ax"
    .globl _start
_start:
    b       entry                       /* code0 */
    .long   0                           /* code1 */
    .quad   0                           /* text_offset: anywhere 2 MB aligned + 0 */
    .quad   _image_size                 /* image_size (bss included) */
    .quad   0xa                         /* flags: little endian, 4K pages, anywhere in RAM */
    .quad   0, 0, 0                     /* res2..res4 */
    .ascii  "ARM\x64"                   /* magic */
    .long   0                           /* res5 */

entry:
    mov     x19, x0                     /* the device tree */
    msr     daifset, #0xf               /* interrupts off */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    at_el1
    /* EL2 -> EL1: 64-bit EL1, the timers and counter usable from EL1 */
    mov     x1, #(1 << 31)              /* HCR_EL2.RW */
    msr     hcr_el2, x1
    mov     x1, #3
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    mov     x1, #0x0800
    movk    x1, #0x30d0, lsl #16        /* SCTLR_EL1 RES1 bits, MMU off */
    msr     sctlr_el1, x1
    mov     x1, #0x3c5                  /* EL1h, DAIF masked */
    msr     spsr_el2, x1
    adr     x1, at_el1
    msr     elr_el2, x1
    eret
at_el1:
    mov     x1, #(3 << 20)              /* CPACR_EL1.FPEN: FP/SIMD on */
    msr     cpacr_el1, x1
    isb
    adr     x20, _start                 /* where we were loaded */
    adrp    x1, _stack_top
    add     x1, x1, :lo12:_stack_top
    mov     sp, x1
    /* clear .bss */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:  /* relocate: *(base + r_offset) = base + r_addend for each R_AARCH64_RELATIVE */
    adrp    x1, __rela_start
    add     x1, x1, :lo12:__rela_start
    adrp    x2, __rela_end
    add     x2, x2, :lo12:__rela_end
3:  cmp     x1, x2
    b.hs    4f
    ldp     x3, x4, [x1], #16           /* r_offset, r_info */
    ldr     x5, [x1], #8                /* r_addend */
    cmp     w4, #1027                   /* R_AARCH64_RELATIVE */
    b.ne    3b
    add     x5, x5, x20
    str     x5, [x20, x3]
    b       3b
4:  adr     x1, vectors
    msr     vbar_el1, x1
    isb
    mov     x0, x19
    mov     x1, x20
    bl      arm_main
5:  wfe
    b       5b

/* exception vectors: everything goes to arm_exception(kind, frame) */
    .macro  vent kind
    .balign 0x80
    sub     sp, sp, #(34 * 8)
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x0, elr_el1
    stp     x30, x0, [sp, #240]
    mrs     x0, spsr_el1
    str     x0, [sp, #256]
    mov     x0, #\kind
    mov     x1, sp
    b       exc_common
    .endm

    .balign 0x800
vectors:
    vent 0  /* EL1t sync */
    vent 1
    vent 2
    vent 3
    vent 4  /* EL1h sync */
    vent 5  /* EL1h irq */
    vent 6
    vent 7
    vent 8  /* EL0 sync */
    vent 9
    vent 10
    vent 11
    vent 12
    vent 13
    vent 14
    vent 15

/* the FP/SIMD registers too: the compiler uses them in ordinary code (copies, pixel
 * loops), and the tick may switch to another thread that does - without them a
 * preempted thread (Linux's or the shell's) found its q0-q31 changed under it */
exc_common:
    sub     sp, sp, #528
    stp     q0, q1, [sp, #0]
    stp     q2, q3, [sp, #32]
    stp     q4, q5, [sp, #64]
    stp     q6, q7, [sp, #96]
    stp     q8, q9, [sp, #128]
    stp     q10, q11, [sp, #160]
    stp     q12, q13, [sp, #192]
    stp     q14, q15, [sp, #224]
    stp     q16, q17, [sp, #256]
    stp     q18, q19, [sp, #288]
    stp     q20, q21, [sp, #320]
    stp     q22, q23, [sp, #352]
    stp     q24, q25, [sp, #384]
    stp     q26, q27, [sp, #416]
    stp     q28, q29, [sp, #448]
    stp     q30, q31, [sp, #480]
    mrs     x2, fpsr
    mrs     x3, fpcr
    str     x2, [sp, #512]
    str     x3, [sp, #520]
    bl      arm_exception
    ldr     x2, [sp, #512]
    ldr     x3, [sp, #520]
    msr     fpsr, x2
    msr     fpcr, x3
    ldp     q0, q1, [sp, #0]
    ldp     q2, q3, [sp, #32]
    ldp     q4, q5, [sp, #64]
    ldp     q6, q7, [sp, #96]
    ldp     q8, q9, [sp, #128]
    ldp     q10, q11, [sp, #160]
    ldp     q12, q13, [sp, #192]
    ldp     q14, q15, [sp, #224]
    ldp     q16, q17, [sp, #256]
    ldp     q18, q19, [sp, #288]
    ldp     q20, q21, [sp, #320]
    ldp     q22, q23, [sp, #352]
    ldp     q24, q25, [sp, #384]
    ldp     q26, q27, [sp, #416]
    ldp     q28, q29, [sp, #448]
    ldp     q30, q31, [sp, #480]
    add     sp, sp, #528
    ldp     x30, x0, [sp, #240]
    msr     elr_el1, x0
    ldr     x0, [sp, #256]
    msr     spsr_el1, x0
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #(34 * 8)
    eret
