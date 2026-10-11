/*
 * sched.c - threads on 64-bit ARM, one core, preemptive: a thread runs until it blocks
 * (a semaphore, a mutex, a join), sleeps or yields - or until the 10 ms timer tick
 * (gic.c) finds another thread ready.  Linux (linux.c) runs on these threads: its kernel
 * threads, its timers, the interrupt poller and the input bridges; a driver that spins
 * on a register no longer stops the shell.
 *
 * Every operation on the scheduler's state runs with interrupts masked.  Each thread
 * keeps its own interrupt mask across a switch (sw() saves and restores DAIF), so a
 * thread preempted inside the timer interrupt resumes there and returns from it.
 */
#include "arm.h"
#include "sched.h"

#define T_READY T_RUNNABLE
#define T_SLEEP T_SLEEPING
#define T_DONE  T_DEAD

void arm_switch(u64 *save_sp, u64 new_sp);
void arm_thread_start(void);

static thr_t main_thr = { .state = T_READY, .name = "shell", .next = &main_thr, .on_cpu = 0 };
u64 kernel_l1_phys;                  /* mmu.c: the kernel's own tables (TTBR0 of kernel threads) */
static thr_t *cur = &main_thr;
static int nthreads = 1;
static u64 switches;
static u64 last_switch, idle_us, idle_total;     /* k_now_us() of the last switch; time in wfi */
static int main_boost;
static volatile u64 wake_gen;        /* thr_wake()s: the idle loop looks again */               /* the shell was woken: it runs next (the UI stays responsive) */
static void account(void) { u64 now = k_now_us(); cur->run_us += now - last_switch; last_switch = now; }

thr_t *thr_self(void) { return cur; }
const char *thr_name(thr_t *t) { return t ? t->name : NULL; }
int thr_count(void) { return nthreads; }
u64 thr_switches(void) { return switches; }
int thr_is_main(void) { return cur == &main_thr; }

/* finished detached threads: their stacks are freed and they leave the ring (the
 * structure stays: a program's records may still point at it) */
static thr_t *rr_from;
static void reap(void) {
    thr_t *prev = &main_thr, *t = main_thr.next;
    while (t != &main_thr) {
        thr_t *next = t->next;
        if (t->state == T_DONE && t->detached && !t->reaped && t != cur) {
            t->reaped = 1;
            kfree(t->stack);
            t->stack = NULL;
            prev->next = next;
            nthreads--;
            if (rr_from == t) rr_from = NULL;
        } else prev = t;
        t = next;
    }
}

/* switch to t; we come back here when something switches to us again */
static u64 slice_from, long_us;
static const char *long_name;
/* the longest a thread ran without a switch since the last call (interrupts masked, or
 * a tick that found nothing else ready): what held the shell up */
void thr_longest(const char **name, u64 *us) {
    u64 f = irq_save();
    *name = long_name ? long_name : "-"; *us = long_us;
    long_us = 0; long_name = NULL;
    irq_restore(f);
}
/* where the shell's time off the CPU went since the last call: blocked (and where), asleep,
 * or ready but waiting - for the "shell: stalled" line (hal.c) */
static u64 main_off_at, main_off_state, main_t[3], main_site[8];
void thr_main_off(u64 *blocked, u64 *sleeping, u64 *waiting, u64 *site) {
    u64 f = irq_save();
    *blocked = main_t[2]; *sleeping = main_t[1]; *waiting = main_t[0];
    for (int i = 0; i < 8; i++) site[i] = main_site[i];
    main_t[0] = main_t[1] = main_t[2] = 0;
    irq_restore(f);
}
static void sw(thr_t *t) {
    thr_t *prev = cur;
    if (prev == &main_thr) {
        main_off_at = k_now_us();
        main_off_state = prev->state == T_BLOCKED ? 2 : prev->state == T_SLEEP ? 1 : 0;
        if (main_off_state == 2) {                                 /* the callers that blocked it */
            u64 *fp = __builtin_frame_address(0);
            for (int i = 0; i < 8 && fp; i++) { main_site[i] = fp[1]; fp = (u64 *)fp[0]; }
        }
    } else if (t == &main_thr && main_off_at) main_t[main_off_state] += k_now_us() - main_off_at;
    u64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    account();
    u64 now = k_now_us();                                       /* the longest run without a switch */
    if (prev != &main_thr && now - slice_from > long_us) { long_us = now - slice_from; long_name = prev->name; }
    slice_from = now;
    cur = t;
    switches++;
    /* a program's thread: its address space and thread pointer */
    prev->fs_base = SYSREG_R(tpidr_el0);
    SYSREG_W(tpidr_el0, t->fs_base);
    if (t->cr3 != prev->cr3) {
        u64 ttbr = t->cr3 ? t->cr3 : kernel_l1_phys;
        __asm__ volatile("msr ttbr0_el1, %0; isb" : : "r"(ttbr) : "memory");
        if (t->cr3 && t->cr3 != kernel_l1_phys && !(t->cr3 >> 48)) __asm__ volatile("tlbi aside1, %0; dsb nsh; isb" : : "r"(0ull) : "memory");   /* out of ASIDs: the shared one */
    }
    arm_switch(&prev->sp, t->sp);
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
    reap();
}

/* the next ready thread after the current one, waking sleepers that are due */
static thr_t *pick(u64 *soonest) {
    u64 now = k_now_us();
    *soonest = ~0ull;
    if (main_thr.state == T_SLEEP && main_thr.wake_at <= now) { main_thr.state = T_READY; main_boost = 1; }
    if (main_boost && main_thr.state == T_READY) {
        main_boost = 0;
        if (cur != &main_thr) rr_from = cur;                      /* the turn the shell interrupted */
        return &main_thr;
    }
    /* round robin goes on from the thread the shell's boost interrupted, not from the
     * shell: otherwise whatever follows the shell in the ring always comes first and the
     * threads between (Linux's timer thread, behind a busy Linux thread) never run */
    thr_t *t = (cur == &main_thr && rr_from) ? rr_from->next : cur->next;
    rr_from = NULL;
    /* QRT's own threads first - Linux's (its drivers: Wi-Fi, the display, storage), the
     * sound, the network - then programs': a busy page in Ladybird (dozens of threads)
     * kept a Linux thread holding Linux's CPU waiting for its turn, and the shell behind
     * it for seconds (0.31.1) */
    thr_t *user = NULL;
    for (int i = 0; i < nthreads; i++, t = t->next) {
        if (t->state == T_SLEEP) {
            if (t->wake_at <= now) t->state = T_READY;
            else if (t->wake_at < *soonest) *soonest = t->wake_at;
        }
        if (t->state == T_READY) {
            if (!t->proc) return t;
            if (!user) user = t;
        }
    }
    return user;
}

/* interrupts masked: run the next ready thread (maybe the current one); with none, idle
 * until the earliest sleeper is due */
static void schedule_inner(void);
static thr_t *bkl_owner;
static int bkl_depth;
static void bkl_release(void) {
    bkl_owner = NULL;
    bkl_depth = 0;
    thr_t *t = &main_thr;
    do { if (t->bkl_waiting) { t->bkl_waiting = 0; if (t->state == T_BLOCKED) { t->state = T_READY; if (t == &main_thr) main_boost = 1; } } t = t->next; } while (t != &main_thr);
}
/* interrupts masked: wait for the big lock and take it at depth d */
static void bkl_take(int d) {
    while (bkl_owner && bkl_owner != cur) {
        cur->bkl_waiting = 1;
        cur->state = T_BLOCKED;
        schedule_inner();
    }
    cur->bkl_waiting = 0;
    bkl_owner = cur;
    bkl_depth = d;
}
void bkl_lock(void) {
    u64 f = irq_save();
    if (bkl_owner == cur) bkl_depth++;
    else bkl_take(1);
    irq_restore(f);
}
void bkl_unlock(void) {
    u64 f = irq_save();
    if (bkl_owner == cur && --bkl_depth == 0) bkl_release();
    irq_restore(f);
}
int bkl_held(void) { return bkl_owner == cur; }
thr_t *bkl_owner_thr(void) { return bkl_owner; }

/* a thread that blocks, sleeps or yields gives the big lock up meanwhile (the tick's
 * preemption does not: the holder is in the middle of something) */
static void schedule(void) {
    int d = 0;
    if (bkl_owner == cur) { d = bkl_depth; bkl_release(); }
    schedule_inner();
    if (d) bkl_take(d);
}
static void schedule_inner(void) {
    for (;;) {
        u64 soonest;
        thr_t *t = pick(&soonest);
        if (t) { if (t != cur) sw(t); return; }
        account();
        u64 gic_ticks(void);
        void gic_wake_in(u64 us);
        u64 wg = wake_gen;
        if (gic_ticks())
            while (k_now_us() < soonest && wake_gen == wg) {      /* the timer wakes it: the tick or sooner; or a wakeup (a helper core's signal) */
                u64 now = k_now_us();
                if (soonest > now) gic_wake_in(soonest - now);
                __asm__ volatile("wfi");
                /* interrupts are masked here: take what woke the core by hand - a helper
                 * core's signal (a program's run ended) wakes its thread now, not at the tick */
                void gic_irq(void);
                gic_irq();
            }
        else while (k_now_us() < soonest) __asm__ volatile("yield");    /* no tick (yet): watch the clock */
        u64 now = k_now_us();
        idle_us += now - last_switch;
        idle_total += now - last_switch;
        last_switch = now;
    }
}

/* the timer tick (gic.c, interrupts masked): another ready thread gets the CPU */
/* The shell preempted in the middle of a frame comes back at the next tick, not after a
 * whole round of every ready thread (Ladybird has dozens: the shell stalled for most of a
 * second at a time, 0.30.2) - the others get one slice in turn between its frames. */
void thr_tick(void) {
    void arm_urun_poll(void);
    arm_urun_poll();                                              /* runs on the helper cores that ended (smp.c) */
    u64 soonest;
    thr_t *t = pick(&soonest);
    if (t && t != cur && cur->state == T_READY) {
        if (cur == &main_thr) main_boost = 1;
        sw(t);
    }
}

void thr_yield(void) { u64 f = irq_save(); schedule(); irq_restore(f); }
void thr_sleep_us(u64 us) {
    u64 f = irq_save();
    cur->wake_at = k_now_us() + us;
    cur->state = T_SLEEP;
    schedule();
    irq_restore(f);
}
void thr_sleep_until(u64 us) {
    u64 f = irq_save();
    if (us > k_now_us()) { cur->wake_at = us; cur->state = T_SLEEP; }
    schedule();
    irq_restore(f);
}
void thr_wake(thr_t *t) {
    u64 f = irq_save();
    wake_gen++;
    if (t && (t->state == T_SLEEP || t->state == T_BLOCKED)) { t->state = T_READY; if (t == &main_thr) main_boost = 1; }
    irq_restore(f);
}

void thr_entry(thr_t *t) {
    __asm__ volatile("msr daifclr, #2");                          /* a new thread can be preempted */
    t->fn(t->arg);
    thr_exit();
}

static thr_t *create(const char *name, void (*fn)(void *), void *arg, usize stack, int state) {
    thr_t *t = kalloc(sizeof *t);
    t->stack = kalloc(stack);
    t->fn = fn; t->arg = arg; t->name = name;
    u64 *sp = (u64 *)((usize)(t->stack + stack) & ~15ull) - 20;  /* arm_switch's frame: 160 bytes */
    memset(sp, 0, 160);
    sp[0] = (u64)(usize)t;                                        /* x19 */
    sp[11] = (u64)(usize)arm_thread_start;                        /* x30 */
    t->sp = (u64)(usize)sp;
    t->state = state;
    t->kstack = (u64)(usize)t->stack;
    t->kstack_top = (u64)(usize)t->stack + stack;
    t->on_cpu = -1;
    u64 f = irq_save();
    t->next = cur->next;
    cur->next = t;
    nthreads++;
    irq_restore(f);
    return t;
}

thr_t *thr_create(const char *name, void (*fn)(void *), void *arg, usize stack) { return create(name, fn, arg, stack, T_READY); }
thr_t *thr_create_suspended(const char *name, void (*fn)(void *), void *arg, usize stack) {
    thr_t *t = create(name, fn, arg, stack, T_BLOCKED);
    t->detached = 1;
    return t;
}
void thr_stop(thr_t *t) {
    u64 f = irq_save();
    if (t->state != T_DONE) { t->state = T_DONE; t->detached = 1; }
    if (bkl_owner == t) bkl_release();
    irq_restore(f);
}
int thr_list(thr_t **out, int max) {
    int n = 0;
    u64 f = irq_save();
    thr_t *t = &main_thr;
    do { if (t->state != T_DONE && n < max) out[n++] = t; t = t->next; } while (t != &main_thr);
    irq_restore(f);
    return n;
}

void thr_block(void) { u64 f = irq_save(); cur->state = T_BLOCKED; schedule(); irq_restore(f); }

void thr_exit(void) {
    irq_save();
    cur->state = T_DONE;
    if (cur->joiner && cur->joiner->state == T_BLOCKED) cur->joiner->state = T_READY;
    schedule();
    for (;;) {}                                                   /* never runs again */
}
void thr_detach(thr_t *t) { t->detached = 1; }
int thr_join(thr_t *t) {
    u64 f = irq_save();
    while (t->state != T_DONE) { t->joiner = cur; cur->state = T_BLOCKED; schedule(); }
    t->detached = 1;
    irq_restore(f);
    return 0;
}
/* a thread that faulted: never runs again; the others go on */
void thr_park(void) { irq_save(); cur->state = T_BLOCKED; schedule(); for (;;) {} }

/* where the CPU went since the last report: the shell, idle, and the busiest others */
void thr_report(void) {
    static u64 since;
    u64 f = irq_save();
    account();
    u64 now = k_now_us(), span = now - since;
    if (!since || !span) {                                        /* the first call: counting starts */
        thr_t *t = &main_thr;
        do { t->run_us = 0; t = t->next; } while (t != &main_thr);
        idle_us = 0;
        since = now;
        irq_restore(f);
        return;
    }
    thr_t *top[3] = { 0 };
    u64 others = 0;
    thr_t *t = &main_thr;
    do {
        if (t != &main_thr) {
            others += t->run_us;
            for (int i = 0; i < 3; i++)
                if (!top[i] || t->run_us > top[i]->run_us) { for (int j = 2; j > i; j--) top[j] = top[j - 1]; top[i] = t; break; }
        }
        t = t->next;
    } while (t != &main_thr);
    u64 pct[3], shell = main_thr.run_us * 100 / span, idle = idle_us * 100 / span, rest = others * 100 / span;
    const char *nm[3];
    for (int i = 0; i < 3; i++) { pct[i] = top[i] ? top[i]->run_us * 100 / span : 0; nm[i] = top[i] ? top[i]->name : "-"; }
    t = &main_thr;
    do { t->run_us = 0; t = t->next; } while (t != &main_thr);
    idle_us = 0;
    since = now;
    u64 sw = switches;
    irq_restore(f);
    u64 gic_ticks(void);
    static u64 last_ticks, last_sw;
    u64 tk = gic_ticks();
    {
        void mem_stats(u64 *, u64 *, u64 *);
        u64 ht, hl, po;
        mem_stats(&ht, &hl, &po);
        klog("memory: %llu MB used: pages %llu MB (heap %llu MB reserved, %llu MB live)", (po - (ht - hl)) >> 20, po >> 20, ht >> 20, hl >> 20);
    }
    klog("cpu: %llu s: shell %llu%%, idle %llu%%, %d others %llu%% (top %s %llu%%, %s %llu%%, %s %llu%%); %llu ticks, %llu switches",
         span / 1000000, shell, idle, nthreads - 1, rest, nm[0], pct[0], nm[1], pct[1], nm[2], pct[2], tk - last_ticks, sw - last_sw);
    last_ticks = tk; last_sw = sw;
    int arm_helpers(void);
    if (arm_helpers()) {
        void arm_helpers_stats(u64 span_us, u64 *busy_pct, u64 *runs);
        u64 bp, rn;
        arm_helpers_stats(span, &bp, &rn);
        klog("cpu: %d more cores run programs: %llu%% busy, %llu runs (each ends in a system call, a fault or a slice)", arm_helpers(), bp, rn);
    }
    void fb_stats(u64 *frames, u64 *us);
    u64 fr, fus;
    fb_stats(&fr, &fus);
    u32 fb_underruns(void);
    klog("cpu: display: %llu frames presented, %llu ms each; %u underrun(s)", fr, fr ? fus / fr / 1000 : 0, fb_underruns());
}

/* ---- semaphores and mutexes: a count and a FIFO of waiters ---- */
struct sem { int count; thr_t *head, *tail; };
sem_t *sem_new(int count) { sem_t *s = kalloc(sizeof *s); s->count = count; return s; }
void sem_del(sem_t *s) { kfree(s); }
void sem_down(sem_t *s) {
    u64 f = irq_save();
    while (s->count <= 0) {
        cur->wnext = NULL;
        if (s->tail) s->tail->wnext = cur; else s->head = cur;
        s->tail = cur;
        cur->state = T_BLOCKED;
        schedule();
    }
    s->count--;
    irq_restore(f);
}
void sem_up(sem_t *s) {
    u64 f = irq_save();
    s->count++;
    thr_t *t = s->head;
    if (t) {
        s->head = t->wnext;
        if (!s->head) s->tail = NULL;
        if (t->state == T_BLOCKED) { t->state = T_READY; if (t == &main_thr) main_boost = 1; }
    }
    irq_restore(f);
}

struct mtx { sem_t sem; thr_t *owner; int depth, recursive; };
mtx_t *mtx_new(int recursive) { mtx_t *m = kalloc(sizeof *m); m->sem.count = 1; m->recursive = recursive; return m; }
void mtx_del(mtx_t *m) { kfree(m); }
void mtx_lock(mtx_t *m) {
    if (m->recursive && m->owner == cur) { m->depth++; return; }
    sem_down(&m->sem);
    m->owner = cur;
    m->depth = 1;
}
void mtx_unlock(mtx_t *m) {
    if (--m->depth > 0) return;
    m->owner = NULL;
    sem_up(&m->sem);
}

/* ---- thread-local slots ---- */
static int tls_used[THR_TLS];
int  tls_key_new(void) {
    u64 f = irq_save();
    int k = -1;
    for (int i = 0; i < THR_TLS && k < 0; i++) if (!tls_used[i]) { tls_used[i] = 1; k = i; }
    irq_restore(f);
    return k;
}
void tls_key_del(int k) { if (k >= 0 && k < THR_TLS) tls_used[k] = 0; }
void tls_put(int k, void *v) { if (k >= 0 && k < THR_TLS) cur->tls[k] = v; }
void *tls_fetch(int k) { return k >= 0 && k < THR_TLS ? cur->tls[k] : NULL; }

/* System Monitor: time the boot core spent idle, since boot */
u64 sched_idle_total_us(void) { return idle_total; }
