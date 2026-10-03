/*
 * sched.c - preemptive round-robin scheduler, on every core (0.9.5).
 *
 * Every thread has its own kernel stack and FPU/SSE save area.  The boot
 * core's APIC timer ticks at 1 kHz: it wakes sleepers and preempts the running
 * thread every 10 ms; the other cores tick at 100 Hz, for preemption only.
 * When nothing is runnable a core's idle thread halts it until the next
 * interrupt (the worker cores also take render jobs there, smp_native.c).
 *
 * Who runs where: kernel threads (the shell, drivers, sound, Bluetooth) stay on
 * the boot core, as before.  Threads of programs run on any core, so a browser's
 * processes and threads - JavaScript, layout, painting, decoding - run in
 * parallel.
 *
 * The kernel itself is still serialised, by one big kernel lock (BKL), as the
 * early SMP Linux and BSD kernels were: a core holds it whenever it runs kernel
 * code for a thread - a system call, a page fault, a device interrupt, a kernel
 * thread - and lets it go when it returns to user mode or switches threads.
 * Kernel code written for one core (interrupts off as its lock) stays correct:
 * at most one core is inside it, and a thread that blocks or is preempted gives
 * the lock up, exactly where on one core another thread could have run.  The
 * lock is recursive per core (an interrupt inside a system call takes it again).
 *
 * Not under the lock: the scheduler's own state (slock), the render-job
 * workers, and two IPIs that must work while the lock is held elsewhere:
 * VEC_TLB (reload CR3 after another core changed page tables: tlb_shootdown)
 * and VEC_RESCHED (switch: a thread became runnable, or the running one was
 * stopped).  Cores waiting for either lock keep answering TLB requests, so a
 * shootdown never waits on a core that waits on its sender.
 */
#include "sched.h"

extern void switch_context(u64 *save_rsp, u64 new_rsp);
extern void thread_start(void);

static thread_t *threads;        /* circular list of every thread but the idle ones */
static thread_t *rr;             /* round-robin cursor */
static int smp_sched;            /* other cores take threads */
#define QUANTUM 10
/* a thread woken by thread_wake() runs at the next 1 ms tick, not the next 10 ms quantum: a
 * frame in Ladybird passes through several processes (input, the UI, WebContent, the shell),
 * and each hand-over used to wait for the quantum */
static volatile int wake_pending;

thread_t *thread_current(void) { percpu_t *c = this_cpu(); return c ? c->cur : NULL; }
u64 sched_idle_ticks(void) { return cpus[0] ? cpus[0]->idle_ticks : 0; }

static void fxsave(void *p)  { __asm__ volatile("fxsave64 (%0)" : : "r"(p) : "memory"); }
static void fxrstor(void *p) { __asm__ volatile("fxrstor64 (%0)" : : "r"(p) : "memory"); }

/* ---- IPIs that work without the big lock ---------------------------------------- */
static void poll_ipis(percpu_t *c) {
    if (c->tlb_req) { write_cr3(read_cr3()); __atomic_store_n(&c->tlb_req, 0, __ATOMIC_RELEASE); }
}

/* Other cores that have address space cr3 loaded reload it: page tables changed. */
void tlb_shootdown(u64 cr3) {
    if (!smp_sched) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    percpu_t *me = this_cpu();
    u32 sent = 0;
    for (int i = 0; i < ncpus; i++) {
        percpu_t *c = cpus[i];
        if (!c || c == me || !c->sched_on || c->loaded_cr3 != cr3) continue;
        __atomic_store_n(&c->tlb_req, 1, __ATOMIC_SEQ_CST);
        lapic_send_ipi(c->apic_id, VEC_TLB);
        sent |= 1u << i;
    }
    if (!sent) return;
    u64 end = k_now_ms() + 200;
    for (int i = 0; i < ncpus; i++) {
        if (!(sent & (1u << i))) continue;
        while (__atomic_load_n(&cpus[i]->tlb_req, __ATOMIC_ACQUIRE)) {
            poll_ipis(me);
            pause();
            if (k_now_ms() > end) { klog("sched: core %d did not answer a TLB shootdown", i); break; }
        }
    }
}

void cpu_load_cr3(u64 cr3) {
    percpu_t *c = this_cpu();
    c->loaded_cr3 = cr3;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    write_cr3(cr3);
}

/* ---- the big kernel lock --------------------------------------------------------- */
static volatile int bkl_owner = -1;

void bkl_lock(void) {
    u64 fl = irq_save();
    percpu_t *c = this_cpu();
    if (bkl_owner == (int)c->index) { c->bkl_depth++; irq_restore(fl); return; }
    for (;;) {
        int expect = -1;
        if (__atomic_compare_exchange_n(&bkl_owner, &expect, (int)c->index, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        /* wait with interrupts as the caller had them; this thread may move to another core meanwhile */
        irq_restore(fl);
        while (__atomic_load_n(&bkl_owner, __ATOMIC_RELAXED) != -1) { poll_ipis(this_cpu()); pause(); }
        fl = irq_save();
        c = this_cpu();
        if (bkl_owner == (int)c->index) { c->bkl_depth++; irq_restore(fl); return; }   /* an interrupt took and left it: impossible, but cheap */
    }
    c->bkl_depth = 1;
    irq_restore(fl);
}

void bkl_unlock(void) {
    u64 fl = irq_save();
    percpu_t *c = this_cpu();
    if (bkl_owner == (int)c->index && --c->bkl_depth == 0) __atomic_store_n(&bkl_owner, -1, __ATOMIC_RELEASE);
    irq_restore(fl);
}

int bkl_held(void) { percpu_t *c = this_cpu(); return c && bkl_owner == (int)c->index; }

/* give the lock up entirely (switching threads); returns the depth to take back */
static int bkl_release_all(percpu_t *c) {
    if (bkl_owner != (int)c->index) return 0;
    int d = c->bkl_depth;
    c->bkl_depth = 0;
    __atomic_store_n(&bkl_owner, -1, __ATOMIC_RELEASE);
    return d;
}
static void bkl_reacquire(int depth) {
    if (!depth) return;
    bkl_lock();
    this_cpu()->bkl_depth = depth;
}

/* ---- the scheduler's own lock ---------------------------------------------------- */
static volatile int slock;
static void slock_get(void) { while (__atomic_exchange_n(&slock, 1, __ATOMIC_ACQUIRE)) { while (slock) { poll_ipis(this_cpu()); pause(); } } }
static void slock_put(void) { __atomic_store_n(&slock, 0, __ATOMIC_RELEASE); }

static int may_run(thread_t *t, percpu_t *c) { return c->index == 0 || t->proc != NULL; }

/* under slock */
static thread_t *pick_next(percpu_t *c) {
    thread_t *cur = c->cur;
    if (threads) {
        thread_t *start = rr ? rr->next : threads, *t = start;
        for (int i = 0; i < 4096; i++) {
            if (t->state == T_RUNNABLE && t->on_cpu < 0 && may_run(t, c)) { rr = t; return t; }
            t = t->next;
            if (t == start) break;
        }
    }
    if (cur != c->idle && cur->state == T_RUNNABLE) return cur;
    return c->idle;
}

/* back on a core after switch_context: the previous thread may now run elsewhere */
static void finish_switch(void) {
    percpu_t *c = this_cpu();
    thread_t *prev = c->prev;
    c->prev = NULL;
    if (prev && prev != c->cur) __atomic_store_n(&prev->on_cpu, -1, __ATOMIC_RELEASE);
    slock_put();
}

/* Switch to the next runnable thread.  Interrupts must be disabled. */
static void schedule(void) {
    percpu_t *c = this_cpu();
    if (c->in_job && c->cur == c->idle) { c->need_resched = 1; return; }   /* finish the render job first */
    int depth = bkl_release_all(c);
    slock_get();
    c->need_resched = 0;
    thread_t *prev = c->cur, *next = pick_next(c);
    if (next == prev) { slock_put(); bkl_reacquire(depth); return; }
    next->on_cpu = (int)c->index;
    c->tss.rsp0 = next->kstack_top;
    c->kernel_rsp = next->kstack_top;
    c->cur = next;
    c->prev = prev;
    if (next->cr3 != c->loaded_cr3) cpu_load_cr3(next->cr3);
    if (next->fs_base != prev->fs_base) wrmsr(MSR_FS_BASE, next->fs_base);
    fxsave(prev->fpu);
    switch_context(&prev->rsp, next->rsp);
    /* back in 'prev', perhaps on another core */
    finish_switch();
    fxrstor(this_cpu()->cur->fpu);
    bkl_reacquire(depth);
}

/* first activation of a thread (entry.S thread_start): finish the switch, and kernel code needs the lock */
void thread_first_run(void) {
    finish_switch();
    percpu_t *c = this_cpu();
    fxrstor(c->cur->fpu);                          /* its own FPU state, not the last thread's on this core */
    if (c->cur != c->idle) bkl_lock();
}

/* other cores that could take a newly runnable program thread: wake an idle one */
static void kick(thread_t *t) {
    if (!smp_sched || !t->proc) { wake_pending = 1; return; }
    percpu_t *me = this_cpu();
    for (int i = 1; i < ncpus; i++) {
        percpu_t *c = cpus[i];
        if (!c || !c->sched_on || c->cur != c->idle) continue;
        c->need_resched = 1;
        if (c != me) lapic_send_ipi(c->apic_id, VEC_RESCHED);
        return;
    }
    wake_pending = 1;                    /* every core is busy: the boot core's next tick */
}

static void on_tick(frame_t *f) {
    (void)f;
    percpu_t *c = this_cpu();
    thread_t *cur = c->cur;
    if (cur == c->idle) c->idle_ticks++; else { c->busy_ticks++; cur->cpu_ticks += c->index ? QUANTUM : 1; }
    if (c->index) { schedule(); return; }          /* other cores: 100 Hz, a quantum each tick */
    ticks++;
    int woke = 0;
    slock_get();
    thread_t *t = threads;
    if (t) do {
        if (t->state == T_SLEEPING && ticks >= t->wake_tick) { t->state = T_RUNNABLE; woke = 1; if (t->proc) kick(t); }
        t = t->next;
    } while (t != threads);
    slock_put();
    if (wake_pending) { woke = 1; wake_pending = 0; }
    if (woke || ticks % QUANTUM == 0) schedule();
}

static void on_resched(frame_t *f) { (void)f; schedule(); }

static void link(thread_t *t) {
    u64 fl = irq_save();
    slock_get();
    if (!threads) { threads = t; t->next = t; }
    else { t->next = threads->next; threads->next = t; }
    slock_put();
    irq_restore(fl);
}

static thread_t *make_thread(const char *name, void (*fn)(void *), void *arg, u64 cr3) {
    thread_t *t = heap_alloc(sizeof *t);
    usize sz = 64 * 1024;
    t->kstack = (u64)(usize)heap_alloc(sz);
    t->kstack_top = (t->kstack + sz) & ~15ull;
    t->name = name;
    t->cr3 = cr3 ? cr3 : kernel_cr3();
    t->on_cpu = -1;
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
    return t;
}

thread_t *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3) {
    thread_t *t = make_thread(name, fn, arg, cr3);
    t->state = T_RUNNABLE;
    link(t);
    return t;
}

/* not runnable until thread_wake(): for threads whose fields the caller fills in first */
thread_t *thread_create_suspended(const char *name, void (*fn)(void *), void *arg, u64 cr3) {
    thread_t *t = make_thread(name, fn, arg, cr3);
    t->state = T_BLOCKED;
    link(t);
    return t;
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) sti_hlt();
}

void sched_init(void) {
    percpu_t *c = this_cpu();
    thread_t *shell = heap_alloc(sizeof *shell);
    u64 rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    shell->kstack_top = (rsp + 4096) & ~4095ull;   /* approximate: the boot stack */
    shell->name = "shell";
    shell->cr3 = kernel_cr3();
    shell->state = T_RUNNABLE;
    shell->on_cpu = 0;
    c->loaded_cr3 = read_cr3();
    c->cur = shell;
    c->idle = make_thread("idle", idle_loop, NULL, 0);
    c->idle->state = T_RUNNABLE;
    c->sched_on = 1;
    link(shell);
    irq_register(VEC_TIMER, on_tick);
    irq_register(VEC_RESCHED, on_resched);
    bkl_lock();                                    /* the shell runs kernel code */
}

/* ---- the other cores ---------------------------------------------------------------------- */
/* Called on each worker core (smp_native.c) once the boot core has set up user mode:
 * from here its idle loop is a thread, and it runs program threads besides render jobs. */
void proc_cpu_setup(void);
void sched_ap_join(void (*idle_fn)(void *)) {
    percpu_t *c = this_cpu();
    proc_cpu_setup();
    bkl_lock();                                    /* the heap is kernel state */
    thread_t *idle = heap_alloc(sizeof *idle);     /* this context: the core's own stack */
    bkl_unlock();
    u64 rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    idle->kstack_top = (rsp + 4096) & ~4095ull;
    idle->name = "idle";
    idle->cr3 = kernel_cr3();
    idle->state = T_RUNNABLE;
    idle->on_cpu = (int)c->index;
    c->idle = c->cur = idle;
    c->loaded_cr3 = read_cr3();
    c->tss.rsp0 = c->kernel_rsp = idle->kstack_top;
    c->sched_on = 1;
    lapic_timer_start_ap(100);
    idle_fn(NULL);
}

void sched_smp_enable(void) { smp_sched = 1; }
int  sched_smp_enabled(void) { return smp_sched; }

/* the idle thread of a worker core: is there a program thread for it? */
int sched_ap_has_work(void) {
    percpu_t *c = this_cpu();
    if (c->need_resched) return 1;
    thread_t *t = threads;
    if (t) do { if (t->state == T_RUNNABLE && t->on_cpu < 0 && t->proc) return 1; t = t->next; } while (t != threads);
    return 0;
}
void sched_ap_run(void) { u64 fl = irq_save(); schedule(); irq_restore(fl); }

/* Every core leaves address space cr3 (its threads were stopped): before it is freed.
 * Called with the big lock held; gives it up while waiting, so a core that is about to
 * notice its thread is gone can take it. */
void sched_quiesce_cr3(u64 cr3) {
    if (!smp_sched) return;
    percpu_t *me = this_cpu();
    u64 end = k_now_ms() + 2000;
    for (;;) {
        int busy = 0;
        for (int i = 0; i < ncpus; i++) {
            percpu_t *c = cpus[i];
            if (!c || c == me || !c->sched_on || c->loaded_cr3 != cr3) continue;
            busy = 1;
            c->need_resched = 1;
            lapic_send_ipi(c->apic_id, VEC_RESCHED);
        }
        if (!busy) return;
        if (k_now_ms() > end) { klog("sched: an address space stayed in use on another core"); return; }
        u64 fl = irq_save();
        int d = bkl_release_all(this_cpu());
        irq_restore(fl);
        for (int i = 0; i < 200; i++) { poll_ipis(this_cpu()); pause(); }
        bkl_reacquire(d);
        me = this_cpu();
    }
}

/* a thread of a stopped process: never runs again; if it is on another core right now, that core switches away */
void thread_stop(thread_t *t) {
    u64 fl = irq_save();
    t->state = T_DEAD;
    int cpu = t->on_cpu;
    percpu_t *c = cpu >= 0 && cpu < ncpus ? cpus[cpu] : NULL;
    if (c && c != this_cpu()) { c->need_resched = 1; lapic_send_ipi(c->apic_id, VEC_RESCHED); }
    irq_restore(fl);
}

void thread_sleep_ms(u64 ms) {
    u64 fl = irq_save();
    thread_t *me = this_cpu()->cur;
    me->wake_tick = ticks + (ms ? ms : 1);
    me->state = T_SLEEPING;
    schedule();
    irq_restore(fl);
}

void thread_yield(void) { u64 fl = irq_save(); schedule(); irq_restore(fl); }

void thread_block(void) {
    u64 fl = irq_save();
    this_cpu()->cur->state = T_BLOCKED;
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
    slock_get();
    int woke = 0;
    if (t->state == T_BLOCKED || t->state == T_SLEEPING) { t->state = T_RUNNABLE; woke = 1; }
    slock_put();
    if (woke) kick(t);
    irq_restore(fl);
}

void thread_exit(void) {
    cli();
    this_cpu()->cur->state = T_DEAD;   /* reclaimed lazily: stacks stay until reused */
    schedule();
    for (;;) hlt();
}

int sched_threads(thread_t **out, int max) {
    int n = 0;
    u64 fl = irq_save();
    slock_get();
    thread_t *t = threads;
    if (t) do { if (n < max && t->state != T_DEAD) out[n++] = t; t = t->next; } while (t != threads);
    slock_put();
    irq_restore(fl);
    return n;
}

/* the IPI handlers that run without the big lock (cpu.c) */
void sched_tlb_ipi(void) { poll_ipis(this_cpu()); }
