/*
 * sched.c - preemptive round-robin scheduler for the boot core.
 *
 * Every thread has its own kernel stack and FPU/SSE save area.  The APIC
 * timer ticks at 1 kHz: it wakes sleepers and preempts the running thread
 * every 10 ms.  When nothing is runnable the idle thread halts the CPU
 * until the next interrupt - the tablet actually rests between frames now.
 * Worker cores do not run threads; they take render jobs (smp_native.c).
 */
#include "sched.h"

extern void switch_context(u64 *save_rsp, u64 new_rsp);
extern void thread_start(void);

static thread_t *threads;        /* circular list */
static thread_t *current, *idle;
static u64 idle_ticks;
#define QUANTUM 10
/* a thread woken by thread_wake() runs at the next 1 ms tick, not the next 10 ms quantum: a
 * frame in Ladybird passes through several processes (input, the UI, WebContent, the shell),
 * and each hand-over used to wait for the quantum */
static volatile int wake_pending;

thread_t *thread_current(void) { return current; }
u64 sched_idle_ticks(void) { return idle_ticks; }

static void fxsave(void *p)  { __asm__ volatile("fxsave64 (%0)" : : "r"(p) : "memory"); }
static void fxrstor(void *p) { __asm__ volatile("fxrstor64 (%0)" : : "r"(p) : "memory"); }

static thread_t *pick_next(void) {
    thread_t *t = current->next;
    for (int i = 0; t && i < 1024; i++, t = t->next) {
        if (t != idle && t->state == T_RUNNABLE) return t;
        if (t == current) break;
    }
    if (current != idle && current->state == T_RUNNABLE) return current;
    return idle;
}

/* Switch to the next runnable thread.  Interrupts must be disabled. */
static void schedule(void) {
    thread_t *prev = current, *next = pick_next();
    if (next == prev) return;
    percpu_t *c = this_cpu();
    c->tss.rsp0 = next->kstack_top;
    c->kernel_rsp = next->kstack_top;
    c->cur = next;
    if (next->cr3 != prev->cr3) write_cr3(next->cr3);
    if (next->fs_base != prev->fs_base) wrmsr(MSR_FS_BASE, next->fs_base);
    current = next;
    fxsave(prev->fpu);
    switch_context(&prev->rsp, next->rsp);
    fxrstor(current->fpu);          /* back in 'prev', now current again */
}

static void on_tick(frame_t *f) {
    (void)f;
    ticks++;
    current->cpu_ticks++;
    if (current == idle) idle_ticks++;
    int woke = 0;
    thread_t *t = threads;
    do {
        if (t->state == T_SLEEPING && ticks >= t->wake_tick) { t->state = T_RUNNABLE; woke = 1; }
        t = t->next;
    } while (t != threads);
    if (wake_pending) { woke = 1; wake_pending = 0; }
    if (woke || ticks % QUANTUM == 0) schedule();
}

static void link(thread_t *t) {
    u64 fl = irq_save();
    if (!threads) { threads = t; t->next = t; }
    else { t->next = threads->next; threads->next = t; }
    irq_restore(fl);
}

thread_t *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3) {
    thread_t *t = heap_alloc(sizeof *t);
    usize sz = 64 * 1024;
    t->kstack = (u64)(usize)heap_alloc(sz);
    t->kstack_top = (t->kstack + sz) & ~15ull;
    t->name = name;
    t->cr3 = cr3 ? cr3 : kernel_cr3();
    /* default FPU state: all exceptions masked */
    fxsave(t->fpu);
    *(u16 *)(t->fpu + 0) = 0x037f;
    *(u32 *)(t->fpu + 24) = 0x1f80;
    /* frame consumed by switch_context: r15 r14 r13 r12 rsi rdi rbp rbx, return address */
    u64 *sp = (u64 *)(usize)(t->kstack_top - 16);
    *--sp = (u64)(usize)thread_start;
    *--sp = 0;                       /* rbx */
    *--sp = 0;                       /* rbp */
    *--sp = 0;                       /* rdi */
    *--sp = 0;                       /* rsi */
    *--sp = (u64)(usize)arg;         /* r12 */
    *--sp = (u64)(usize)fn;          /* r13 */
    *--sp = 0;                       /* r14 */
    *--sp = 0;                       /* r15 */
    t->rsp = (u64)(usize)sp;
    t->state = T_RUNNABLE;
    link(t);
    return t;
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) sti_hlt();
}

void sched_init(void) {
    thread_t *shell = heap_alloc(sizeof *shell);
    u64 rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    shell->kstack_top = (rsp + 4096) & ~4095ull;   /* approximate: the boot stack */
    shell->name = "shell";
    shell->cr3 = kernel_cr3();
    shell->state = T_RUNNABLE;
    current = shell;
    link(shell);
    idle = thread_create("idle", idle_loop, NULL, 0);
    this_cpu()->cur = shell;
    irq_register(VEC_TIMER, on_tick);
}

void thread_sleep_ms(u64 ms) {
    u64 fl = irq_save();
    current->wake_tick = ticks + (ms ? ms : 1);
    current->state = T_SLEEPING;
    schedule();
    irq_restore(fl);
}

void thread_yield(void) { u64 fl = irq_save(); schedule(); irq_restore(fl); }

void thread_block(void) {
    u64 fl = irq_save();
    current->state = T_BLOCKED;
    schedule();
    irq_restore(fl);
}

void thread_wake(thread_t *t) {
    if (!t) {                                    /* a bug in the caller: say who, and carry on */
        static int told;
        if (!told++) { char w[64]; kernel_symbol((u64)(usize)__builtin_return_address(0), w, sizeof w); klog("sched: thread_wake(NULL) from %s", w); }
        return;
    }
    u64 fl = irq_save();
    if (t->state == T_BLOCKED || t->state == T_SLEEPING) { t->state = T_RUNNABLE; wake_pending = 1; }
    irq_restore(fl);
}

void thread_exit(void) {
    cli();
    current->state = T_DEAD;        /* reclaimed lazily: stacks stay until reused */
    schedule();
    for (;;) hlt();
}

int sched_threads(thread_t **out, int max) {
    int n = 0;
    u64 fl = irq_save();
    thread_t *t = threads;
    do { if (n < max && t->state != T_DEAD) out[n++] = t; t = t->next; } while (t != threads);
    irq_restore(fl);
    return n;
}
