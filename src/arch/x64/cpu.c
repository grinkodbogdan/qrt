/*
 * cpu.c - descriptor tables and interrupt dispatch.
 *
 * GDT layout (fixed so SYSCALL/SYSRET can derive user selectors from STAR):
 *   0x00 null   0x08 kernel code   0x10 kernel data   0x18 user code32 (unused)
 *   0x20 user data   0x28 user code64   0x30 TSS (16 bytes)
 */
#include "arch.h"
#include "sched.h"

percpu_t *cpus[MAX_CPUS];
int ncpus;

typedef struct __attribute__((packed)) { u16 lo; u16 sel; u8 ist; u8 attr; u16 mid; u32 hi; u32 zero; } idt_entry_t;
typedef struct __attribute__((packed)) { u16 limit; u64 base; } dtr_t;

static idt_entry_t idt[256] __attribute__((aligned(16)));
static irq_handler_t handlers[256];
extern u64 isr_table[256];

void irq_register(int v, irq_handler_t h) { handlers[v & 255] = h; }

percpu_t *this_cpu(void) {
    percpu_t *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) {
        u64 a = isr_table[v];
        idt[v].lo = (u16)a;
        idt[v].sel = SEL_KCODE;
        idt[v].ist = v == 8 ? 1 : 0;           /* double fault on its own stack */
        idt[v].attr = 0x8e;                    /* present, DPL0, interrupt gate */
        idt[v].mid = (u16)(a >> 16);
        idt[v].hi = (u32)(a >> 32);
        idt[v].zero = 0;
    }
}

void cpu_setup(percpu_t *c, u32 index) {
    c->self = c;
    c->index = index;
    c->gdt[0] = 0;
    c->gdt[1] = 0x00af9a000000ffffull;          /* kernel code 64 */
    c->gdt[2] = 0x00cf92000000ffffull;          /* kernel data */
    c->gdt[3] = 0x00cffa000000ffffull;          /* user code 32 (placeholder for STAR) */
    c->gdt[4] = 0x00cff2000000ffffull;          /* user data */
    c->gdt[5] = 0x00affa000000ffffull;          /* user code 64 */
    u64 t = (u64)(usize)&c->tss;
    u64 lim = sizeof(tss_t) - 1;
    c->gdt[6] = (lim & 0xffff) | ((t & 0xffffff) << 16) | (0x89ull << 40) | (((lim >> 16) & 0xf) << 48) | (((t >> 24) & 0xff) << 56);
    c->gdt[7] = t >> 32;
    c->tss.iomap = sizeof(tss_t);
    c->tss.ist[0] = (u64)(usize)(c->df_stack + sizeof c->df_stack);

    dtr_t g = { sizeof c->gdt - 1, (u64)(usize)c->gdt };
    dtr_t i = { sizeof idt - 1, (u64)(usize)idt };
    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
        "lidt %1\n"
        "movw $0x30, %%ax\n ltr %%ax\n"
        : : "m"(g), "m"(i) : "rax", "memory");
    wrmsr(MSR_GS_BASE, (u64)(usize)c);
    wrmsr(MSR_KERNEL_GS, 0);
    wrmsr(MSR_PAT, (rdmsr(MSR_PAT) & ~0xff00ull) | 0x0100ull);
    write_cr0((read_cr0() & ~4ull) | 2ull);     /* FPU: no emulation, monitor */
    write_cr4(read_cr4() | (1u << 9) | (1u << 10));
    cpus[index] = c;
}

static const char *exc_name(u64 v) {
    static const char *n[] = { "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
        "invalid opcode", "device not available", "double fault", "coprocessor overrun", "invalid TSS",
        "segment not present", "stack fault", "general protection fault", "page fault", "reserved",
        "x87 FP error", "alignment check", "machine check", "SIMD FP error", "virtualization", "control protection" };
    return v < ARRAY_LEN(n) ? n[v] : "exception";
}

/* user-mode faults are handed to the process layer (which kills the process) */
int (*user_fault_hook)(frame_t *f);
int (*page_fault_hook)(frame_t *f);      /* demand paging of user memory */
int (*kernel_fault_hook)(frame_t *f);    /* a kernel fault in a program's system call: stop the program (never returns) */

void (*user_return_hook)(frame_t *f);
int (*user_return_check)(void);          /* is there anything for user_return_hook? (no lock taken) */

void isr_dispatch(frame_t *f) {
    u64 v = f->vector;
    int user = (f->cs & 3) != 0;
    if (v < 32) {
        /* kernel work for a thread: under the big kernel lock (sched.c); a stopped thread goes */
        bkl_lock();
        struct thread *t = thread_current();
        if (user && t && t->state == T_DEAD) thread_exit();
        if (v == 14 && page_fault_hook && page_fault_hook(f)) { bkl_unlock(); return; }
        if (user && user_fault_hook && user_fault_hook(f)) { bkl_unlock(); return; }
        if (!user && kernel_fault_hook) kernel_fault_hook(f);   /* returns only if it cannot help */
        native_panic(exc_name(v), f);
    }
    /* acknowledge first: a handler may switch threads (the timer does) and
     * must not leave this vector in service while another thread runs */
    if (v != VEC_SPURIOUS) lapic_eoi();
    struct thread *t = thread_current();
    if (t) t->irq_depth++;
    if (v == VEC_TLB) sched_tlb_ipi();
    else if (v == VEC_TIMER || v == VEC_RESCHED || v == VEC_WAKE) { if (handlers[v]) handlers[v](f); }   /* the scheduler's own: no big lock */
    else if (handlers[v]) { bkl_lock(); handlers[v](f); bkl_unlock(); }
    if (t) t->irq_depth--;
    if (user && user_return_hook && (!user_return_check || user_return_check())) {                   /* signals, a stopped process: for a thread interrupted in user mode */
        bkl_lock();
        t = thread_current();
        if (t && t->state == T_DEAD) thread_exit();
        user_return_hook(f);
        bkl_unlock();
    }
}
