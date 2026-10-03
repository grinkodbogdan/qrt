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
    subq $32, %rsp
    call thread_first_run       /* finish the switch; kernel code takes the big lock */
    addq $32, %rsp
    sti
    movq %r12, %rcx
    subq $32, %rsp
    call *%r13
    call thread_exit
2:  hlt
    jmp 2b

/* void enter_user(u64 rip, u64 rsp) - first entry of a process into ring 3 */
    .globl enter_user
enter_user:
    cli
    pushq $0x23                 /* user SS (0x20 | 3) */
    pushq %rdx                  /* user RSP */
    pushq $0x202                /* RFLAGS: IF */
    pushq $0x2b                 /* user CS (0x28 | 3) */
    pushq %rcx                  /* RIP */
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    swapgs
    iretq

/* void enter_user_frame(frame_t *f) - start a cloned thread in ring 3 from a
 * copy of its parent's system-call frame (on this thread's kernel stack) */
    .globl enter_user_frame
enter_user_frame:
    cli
    movq %rcx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    swapgs
    iretq

/*
 * SYSCALL lands here: RCX = user RIP, R11 = user RFLAGS, interrupts off
 * (SFMASK).  Build the same frame an interrupt would (so frame_t works for
 * both), call syscall_dispatch(frame) with interrupts on, and return with
 * IRETQ, which also copes with any RIP/RSP the program left behind.
 */
    .globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:16           /* user rsp */
    movq %gs:8, %rsp            /* this thread's kernel stack */
    pushq $0x23                 /* ss */
    pushq %gs:16                /* rsp */
    pushq %r11                  /* rflags */
    pushq $0x2b                 /* cs */
    pushq %rcx                  /* rip */
    pushq $0                    /* err */
    pushq $0x80                 /* "vector" marker */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %r12
    subq $528, %rsp
    andq $-16, %rsp
    fxsave64 (%rsp)
    movq %r12, %rcx
    subq $32, %rsp
    sti
    cld
    call syscall_dispatch
    cli
    addq $32, %rsp
    fxrstor64 (%rsp)
    movq %r12, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    swapgs
    iretq
