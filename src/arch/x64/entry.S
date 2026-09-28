/* entry.S - small pieces of the native kernel that must be assembly.
 * Calling convention: Microsoft x64 (args in rcx, rdx, r8, r9). */
    .text

/* void switch_stack(u64 new_top, void (*fn)(void *), void *arg)  - never returns */
    .globl switch_stack
switch_stack:
    movq %rcx, %rsp
    andq $-16, %rsp
    subq $32, %rsp              /* shadow space; rsp stays 16-byte aligned at the call */
    movq %r8, %rcx
    call *%rdx
1:  cli
    hlt
    jmp 1b

/* void switch_context(u64 *save_rsp, u64 new_rsp)
 * Saves the Microsoft-ABI callee-saved registers on the current stack, stores
 * the stack pointer, and resumes the other thread where it left off. */
    .globl switch_context
switch_context:
    pushq %rbx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rcx)
    movq %rdx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rbx
    ret

/* First activation of a new thread: r13 = entry function, r12 = argument. */
    .globl thread_start
thread_start:
    sti
    movq %r12, %rcx
    subq $32, %rsp
    call *%r13
    call thread_exit
2:  hlt
    jmp 2b
