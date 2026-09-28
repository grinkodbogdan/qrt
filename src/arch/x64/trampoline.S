/*
 * trampoline.S - first instructions of a freshly started CPU core.
 *
 * An application processor wakes in 16-bit real mode at the page named by
 * the STARTUP IPI.  This code is copied to that page (below 1 MiB), switches
 * through 32-bit protected mode into 64-bit long mode on the kernel's page
 * tables, and calls ap_main(arg) on its own stack.
 *
 * Position independence: %ebx/%rbx holds the page's linear address, and the
 * few absolute operands (GDT base, far-jump targets) are patched at copy time
 * by smp_native.c through the exported labels.
 */
    .text
    .globl tramp_start, tramp_end, tramp_gdtr_base, tramp_jmp32, tramp_jmp64
    .globl tramp_cr3, tramp_stack, tramp_entry, tramp_arg, tramp_pm32, tramp_lm64, tramp_gdt

    .code16
tramp_start:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    movzwl %ax, %ebx
    shll $4, %ebx                           /* ebx = linear base of this page */
    lgdtl (tramp_gdtr - tramp_start)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    .byte 0x66, 0xea                        /* ljmpl $0x08, $pm32 (patched) */
tramp_jmp32:
    .long 0
    .word 0x08

    .code32
tramp_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl %cr4, %eax
    orl $0x20, %eax                         /* PAE */
    movl %eax, %cr4
    movl (tramp_cr3 - tramp_start)(%ebx), %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx                  /* EFER.LME */
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl %cr0, %eax
    orl $0x80000001, %eax                   /* PG | PE */
    movl %eax, %cr0
    .byte 0xea                              /* ljmp $0x18, $lm64 (patched) */
tramp_jmp64:
    .long 0
    .word 0x18

    .code64
tramp_lm64:
    movq (tramp_stack - tramp_start)(%rbx), %rsp
    movq (tramp_arg - tramp_start)(%rbx), %rcx
    movq (tramp_entry - tramp_start)(%rbx), %rax
    subq $32, %rsp
    call *%rax
1:  cli
    hlt
    jmp 1b

    .p2align 3
tramp_gdt:
    .quad 0
    .quad 0x00cf9a000000ffff                /* 0x08 code 32 */
    .quad 0x00cf92000000ffff                /* 0x10 data */
    .quad 0x00af9a000000ffff                /* 0x18 code 64 */
tramp_gdtr:
    .word 31
tramp_gdtr_base:
    .long 0                                 /* patched: page + offset of tramp_gdt */
    .p2align 3
tramp_cr3:   .quad 0
tramp_stack: .quad 0
tramp_entry: .quad 0
tramp_arg:   .quad 0
tramp_end:
