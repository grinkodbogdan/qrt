/*
 * smp.c - the other CPU cores on 64-bit ARM, started through PSCI CPU_ON.
 *
 * The kernel - the shell, Linux's drivers, every system call - stays on the boot core,
 * written for one core as it is (interrupts masked as its lock).  The other cores
 * ("helpers") run programs: a program's thread about to return to user mode is handed
 * to a free helper (arm_urun), which loads its address space, thread pointer and
 * registers and runs it at EL0 - until it makes a system call, faults, or its 10 ms
 * slice ends with other threads waiting.  Then the helper hands the registers back and
 * signals the boot core (SGI 2), where the thread - asleep in arm_urun meanwhile - takes
 * the exception as if it had happened there (uentry.c), and is handed out again.  So
 * Ladybird's processes and threads compute on all the cores at once, and the kernel
 * never runs on two (0.32.0).
 *
 * A helper also does the shell's pure pixel work between runs (smp_run: the wallpaper,
 * turning the picture...) - a job must not call the kernel: no heap, no log, no locks.
 */
#include "uarch.h"
#include "../../kernel/smp.h"
#include "../../user/proc.h"

#define MAXCPU 8
extern char secondary_entry[], vectors[];
static struct ctx { u64 sp, ttbr, tcr, mair, sctlr, vbar, entry, idx; } __attribute__((aligned(64))) ctx[MAXCPU];
static volatile u32 alive[MAXCPU];
static u64 mp[MAXCPU];
static int nworkers;
static volatile struct { smp_job_t fn; void *arg; int count; u32 gen; } job;
static volatile int next_idx, done_idx;
static int psci_conduit = -1;                    /* 0 smc, 1 hvc */

static long psci_call(u64 fn, u64 a1, u64 a2, u64 a3) {
    register u64 x0 __asm__("x0") = fn, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
    if (psci_conduit) __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (long)x0;
}
static void clean(const void *p, usize n) {
    for (u64 a = (u64)(usize)p & ~63ull; a < (u64)(usize)p + n; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void run_indices(void) {
    for (;;) {
        int i = __atomic_fetch_add(&next_idx, 1, __ATOMIC_ACQ_REL);
        if (i >= job.count) return;
        job.fn(job.arg, i, job.count);
        __atomic_fetch_add(&done_idx, 1, __ATOMIC_RELEASE);
    }
}

/* how fast a core is: a fixed loop, timed on the counter (the clusters may run at very
 * different clocks - a band given to a slow core holds up the whole frame) */
static volatile u64 speed[MAXCPU];                       /* counter ticks for the loop */
static volatile int use[MAXCPU];                         /* fast enough to take jobs */
static u64 time_once(void);
static u64 time_loop(void) {                              /* the best of three: noise only slows */
    u64 b = time_once();
    for (int i = 0; i < 2; i++) { u64 t = time_once(); if (t < b) b = t; }
    return b;
}
static u64 time_once(void) {
    u64 t0, t1;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
    volatile u32 x = 0;
    for (u32 i = 0; i < 2000000; i++) x += i;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t1));
    return t1 - t0;
}
/* ---- programs on the helper cores ---- */
typedef struct urun {
    frame_t f;                       /* the registers: in, and back out at the exception */
    u64 fs_base, ttbr;               /* TPIDR_EL0, TTBR0 (with the ASID) */
    thr_t *t;
    volatile int state;              /* 0 idle, 1 queued, 2 running, 3 done (on the done list) */
    volatile int stop;               /* the boot core wants it back (a signal, a stop) */
    int core;
    u64 kind, esr, far;              /* what ended the run: 8 sync (esr, far), 9 interrupt */
    struct urun *next;
} urun_t;
static struct helper {
    u64 jb[22];                      /* arm_setjmp: back to the loop from the exception */
    u8 *exc_top;                     /* its exception stack: the frame goes in at the top */
    urun_t *run;
    volatile u64 ttbr;               /* the address space it has loaded (0: the kernel's) */
    u32 gicmask;
    volatile u64 busy_us, runs;
    volatile u64 crash_kind, crash_esr, crash_elr, crash_far;
    int crash_told;
} hc[MAXCPU];
static volatile int qlock;
static urun_t *qhead, *qtail, *done;
static int nhelpers;
static u32 boot_mask;
static void ql(void) { while (__atomic_exchange_n(&qlock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static void qu(void) { __atomic_store_n(&qlock, 0, __ATOMIC_RELEASE); }
int  arm_setjmp(u64 *buf) __attribute__((returns_twice));
void arm_longjmp(u64 *buf, int v) __attribute__((noreturn));
void arm_enter_user(void *area);
u32  gic_my_mask(void);
void gic_sgi(u32 mask, u32 id);
void gic_cpu_init_helper(u32 me);
u32  gic_vtimer(void);
u64  gic_tick_count(void);
u64  gic_cpu_base(void);
u64  gic_dist(void);
extern u64 kernel_l1_phys;

static u64 helper_idx(void) { return SYSREG_R(tpidr_el1); }
static void htimer(int on) {
    if (on) __asm__ volatile("msr cntv_tval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"(gic_tick_count()), "r"(1ull));
    else __asm__ volatile("msr cntv_ctl_el0, %0; isb" : : "r"(0ull));
}
static u64 now_us(void) {
    u64 c, f;
    __asm__ volatile("isb; mrs %0, cntvct_el0; mrs %1, cntfrq_el0" : "=r"(c), "=r"(f));
    return f ? c / (f / 1000000) : 0;
}

/* nothing to do: sleep until a signal (wfi - QEMU spins on wfe), saying so first so the
 * boot core sends one, and looking once more after that */
static volatile u32 idle_mask;
static void helper_sleep(struct helper *h, u32 seen) {
    __atomic_fetch_or(&idle_mask, h->gicmask, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&qhead, __ATOMIC_SEQ_CST) && __atomic_load_n(&job.gen, __ATOMIC_SEQ_CST) == seen) {
        /* a lost signal costs 20 ms, not the program */
        __asm__ volatile("msr cntv_tval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"(gic_tick_count() * 2), "r"(1ull));
        __asm__ volatile("wfi" ::: "memory");
        htimer(0);
    }
    __atomic_fetch_and(&idle_mask, ~h->gicmask, __ATOMIC_SEQ_CST);
    u64 cpu = gic_cpu_base();                             /* the signal: taken by hand (interrupts stay masked) */
    for (int n = 0; n < 16; n++) {
        u32 iar = R32(cpu + 0x0c);
        if ((iar & 0x3ff) >= 1020) break;
        W32(cpu + 0x10, iar);
    }
}
static void wake_helpers(void) {
    u32 m = __atomic_load_n(&idle_mask, __ATOMIC_SEQ_CST);
    if (m) gic_sgi(m, 3);
}

/* a helper's loop: drawing jobs, and programs' threads from the queue */
static void helper_loop(u64 idx) {
    struct helper *h = &hc[idx];
    u32 seen = job.gen;
    for (;;) {
        if (__atomic_load_n(&job.gen, __ATOMIC_ACQUIRE) != seen) { seen = job.gen; if (use[idx]) run_indices(); }   /* drawing: the fast cores */
        urun_t *r = NULL;
        if (__atomic_load_n(&qhead, __ATOMIC_ACQUIRE)) {
            ql();
            if ((r = qhead)) { qhead = r->next; if (!qhead) qtail = NULL; r->state = 2; r->core = (int)idx; h->run = r; }
            qu();
        }
        if (!r) { helper_sleep(h, seen); continue; }
        if (r->t->state == T_DEAD || r->stop) {            /* stopped while it waited: its address space may be gone */
            r->kind = 9; r->esr = r->far = 0;
            ql(); r->state = 3; r->next = done; done = r; h->run = NULL; qu();
            gic_sgi(boot_mask, 2);
            continue;
        }
        h->ttbr = r->ttbr;
        __asm__ volatile("msr ttbr0_el1, %0; isb" : : "r"(r->ttbr) : "memory");
        if (!(r->ttbr >> 48)) __asm__ volatile("tlbi aside1, %0; dsb nsh; isb" : : "r"(0ull) : "memory");   /* the shared ASID */
        SYSREG_W(tpidr_el0, r->fs_base);
        u64 t0 = now_us();
        if (!arm_setjmp(h->jb)) {
            u8 *area = h->exc_top - sizeof(frame_t);
            memcpy(area, &r->f, sizeof(frame_t));
            htimer(1);
            arm_enter_user(area);                         /* comes back by arm_longjmp */
        }
        htimer(0);
        __asm__ volatile("msr ttbr0_el1, %0; isb" : : "r"(kernel_l1_phys) : "memory");   /* off its address space */
        h->ttbr = 0;
        h->busy_us += now_us() - t0;
        h->runs++;
        ql();                                             /* done: the boot core takes it */
        r->state = 3;
        r->next = done;
        done = r;
        h->run = NULL;
        qu();
        gic_sgi(boot_mask, 2);
    }
}

/* main.c: an exception on a helper core - from the program it runs (anything else is a
 * kernel bug there: the core stops) */
void arm_helper_exception(u64 kind, u64 *frame) {
    struct helper *h = &hc[helper_idx()];
    urun_t *r = h->run;
    if (kind < 8 || !r) {                                 /* a kernel bug on this core: say so (arm_urun_poll), stop it */
        h->crash_kind = kind ? kind : 100; h->crash_esr = SYSREG_R(esr_el1); h->crash_elr = SYSREG_R(elr_el1); h->crash_far = SYSREG_R(far_el1);
        __atomic_fetch_and(&idle_mask, ~h->gicmask, __ATOMIC_SEQ_CST);
        if (r) {                                          /* its program goes back, as it was before this run */
            r->kind = 9; r->esr = r->far = 0;
            ql(); r->state = 3; r->next = done; done = r; h->run = NULL; qu();
        }
        gic_sgi(boot_mask, 2);
        for (;;) __asm__ volatile("wfi");
    }
    u64 esr = 0, far = 0;
    if (kind == 8) { esr = SYSREG_R(esr_el1); far = SYSREG_R(far_el1); }
    else if (kind == 9) {
        u64 cpu = gic_cpu_base();
        int timer = 0;
        for (int n = 0; n < 16; n++) {
            u32 iar = R32(cpu + 0x0c), id = iar & 0x3ff;
            if (id >= 1020) break;
            W32(cpu + 0x10, iar);
            if (id == gic_vtimer()) timer = 1;
        }
        if (timer) htimer(1);
        /* the slice ended: go on unless someone waits for this core or wants the thread */
        proc_t *p = r->t->proc;
        int want = r->stop || __atomic_load_n(&qhead, __ATOMIC_RELAXED) || r->t->state == T_DEAD || (p && (p->killed || p->sig_pending)) || r->t->sig_pending
                   || (use[helper_idx()] && __atomic_load_n(&next_idx, __ATOMIC_RELAXED) < job.count);   /* drawing waits */
        if (!want) return;                                /* straight back to the program */
    }
    memcpy(&r->f, (u8 *)frame - 528, sizeof(frame_t));
    r->fs_base = SYSREG_R(tpidr_el0);
    r->kind = kind == 8 ? 8 : 9; r->esr = esr; r->far = far;
    arm_longjmp(h->jb, 1);
}

int arm_helpers(void) { return nhelpers; }
int sched_smp_enabled(void) { return nhelpers > 0; }

/* the boot core: run f's thread on a helper until its next exception there; returns the
 * kind (8: *esr, *far are the exception's; 9: an interrupt - it was preempted).  The
 * thread sleeps meanwhile; a wakeup (a signal, Stop) calls it back early. */
u64 arm_urun(frame_t *f, u64 *esr, u64 *far) {
    thr_t *t = thr_self();
    urun_t *r = t->urun;
    if (!r) {
        u8 *m = kalloc(sizeof(urun_t) + 16);
        r = (urun_t *)(((usize)m + 15) & ~(usize)15);
        t->urun = r;
    }
    memcpy(&r->f, f, sizeof(frame_t));
    r->fs_base = SYSREG_R(tpidr_el0);
    r->ttbr = t->cr3 ? t->cr3 : kernel_l1_phys;
    r->t = t; r->stop = 0; r->next = NULL;
    u64 fl = irq_save();
    ql();
    r->state = 1;
    if (qtail) qtail->next = r; else qhead = r;
    qtail = r;
    qu();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wake_helpers();
    int asked = 0;
    /* interrupts stay masked from here to the block: the boot core takes the helper's
     * signal (and wakes this thread) only once it has switched away - no lost wakeup */
    while (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != 0) {
        thr_block();
        if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) == 0) break;
        if (asked) continue;
        asked = 1;                                        /* woken early: a signal or Stop - take it back */
        ql();
        if (r->state == 1) {                              /* still queued: out of the queue */
            urun_t **pp = &qhead, *prev = NULL;
            while (*pp && *pp != r) { prev = *pp; pp = &(*pp)->next; }
            if (*pp) { *pp = r->next; if (qtail == r) qtail = prev; }
            r->kind = 9; r->esr = r->far = 0;
            r->state = 0;
        } else if (r->state == 2) { r->stop = 1; gic_sgi(hc[r->core].gicmask, 1); }
        qu();
    }
    irq_restore(fl);
    memcpy(f, &r->f, sizeof(frame_t));
    t->fs_base = r->fs_base;
    SYSREG_W(tpidr_el0, r->fs_base);
    *esr = r->esr; *far = r->far;
    return r->kind;
}

/* the boot core (SGI 2, and every tick): threads whose run ended go on */
void arm_urun_poll(void) {
    for (int i = 1; i < MAXCPU; i++)
        if (hc[i].crash_kind && !hc[i].crash_told) {
            hc[i].crash_told = 1;
            extern char _start[];
            klog("smp: core %llx stopped: exception %llu in the kernel at +%llx (esr %llx, address %llx); the others go on",
                 mp[i], hc[i].crash_kind, hc[i].crash_elr - (u64)(usize)_start, hc[i].crash_esr, hc[i].crash_far);
        }
    if (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) return;
    u64 fl = irq_save();
    ql();
    urun_t *d = done;
    done = NULL;
    for (urun_t *r = d; r; r = r->next) __atomic_store_n(&r->state, 0, __ATOMIC_RELEASE);
    qu();
    for (urun_t *r = d, *n; r; r = n) { n = r->next; thr_wake(r->t); }
    irq_restore(fl);
}

/* before an address space is freed: no helper may still have it loaded */
void arm_helpers_quiesce(u64 cr3) {
    if (!nhelpers || !cr3) return;
    u64 fl = irq_save();                                  /* queued runs in it: they never start */
    ql();
    for (urun_t *r = qhead; r; r = r->next) if (!((r->ttbr ^ cr3) & 0xffffffffffffull)) r->stop = 1;
    qu();
    irq_restore(fl);
    u64 end = k_now_ms() + 500;
    for (;;) {
        int busy = 0;
        for (int i = 1; i < MAXCPU; i++) {
            if (!hc[i].exc_top || ((hc[i].ttbr ^ cr3) & 0xffffffffffffull)) continue;
            busy = 1;
            urun_t *r = hc[i].run;
            if (r) r->stop = 1;
            gic_sgi(hc[i].gicmask, 1);
        }
        if (!busy) return;
        if (k_now_ms() > end) { klog("smp: an address space stayed in use on a helper core"); return; }
        for (int i = 0; i < 1000; i++) __asm__ volatile("yield");
    }
}

/* the helpers' share of the time since the last call, in percent of one core each */
void arm_helpers_stats(u64 span_us, u64 *busy_pct, u64 *runs) {
    static u64 last_busy, last_runs;
    u64 b = 0, n = 0;
    for (int i = 1; i < MAXCPU; i++) { b += hc[i].busy_us; n += hc[i].runs; }
    *busy_pct = span_us && nhelpers ? (b - last_busy) * 100 / span_us / (u64)nhelpers : 0;
    *runs = n - last_runs;
    last_busy = b; last_runs = n;
}

/* the boot core's word to each core: 1 run programs, 2 stay parked (it answered too late,
 * or the signals between cores do not reach it) */
static volatile int go[MAXCPU];
static volatile u32 sgi_seen[MAXCPU];
static void secondary_main(u64 idx) {
    SYSREG_W(tpidr_el1, idx);
    SYSREG_W(cntkctl_el1, SYSREG_R(cntkctl_el1) | 2);             /* programs read the virtual counter (as mmu.c's core) */
    hc[idx].gicmask = gic_my_mask();
    gic_cpu_init_helper(hc[idx].gicmask);
    alive[idx] = 1;                                               /* first: a slow clock must not look like a dead core */
    __asm__ volatile("dsb sy; sev" ::: "memory");
    speed[idx] = time_loop();
    /* the signal test: the boot core sends SGI 3; this core answers with SGI 2 */
    u64 cpu = gic_cpu_base();
    while (!__atomic_load_n(&go[idx], __ATOMIC_ACQUIRE)) {
        u32 iar = R32(cpu + 0x0c), id = iar & 0x3ff;
        if (id < 1020) {
            W32(cpu + 0x10, iar);
            if (id == 3 && !sgi_seen[idx]) { __atomic_store_n(&sgi_seen[idx], 1, __ATOMIC_RELEASE); gic_sgi(boot_mask, 2); }
        }
        __asm__ volatile("wfe" ::: "memory");
    }
    if (go[idx] != 1) for (;;) __asm__ volatile("wfi");          /* parked for good */
    helper_loop(idx);
}

int smp_workers(void) { return nworkers; }
int smp_enabled(void) { return nworkers > 0; }
void smp_set_enabled(int on) { (void)on; }
int smp_init(void) { return nworkers; }

static volatile int busy;
void smp_run(smp_job_t fn, void *arg, int count) {
    /* one job at a time: a second caller (another thread) does its own work alone */
    if (!nworkers || count < 2 || __atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < count; i++) fn(arg, i, count);
        return;
    }
    job.fn = fn; job.arg = arg; job.count = count;
    next_idx = 0; done_idx = 0;
    __atomic_fetch_add(&job.gen, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("dsb sy; sev" ::: "memory");
    wake_helpers();
    run_indices();                               /* the boot core works too */
    while (__atomic_load_n(&done_idx, __ATOMIC_ACQUIRE) < count) __asm__ volatile("yield");
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/* every core the tree lists but this one; returns how many answered */
int arm_smp_start(void) {
    int n = fdt_find_compatible(-1, "arm,psci-1.0");
    if (n < 0) n = fdt_find_compatible(-1, "arm,psci-0.2");
    if (n < 0) { klog("smp: no PSCI in the tree - one core"); return 0; }
    int len;
    const char *m = fdt_prop(n, "method", &len);
    psci_conduit = m && !strcmp(m, "hvc");
    u64 me;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(me));
    me &= 0xff00ffffffull;
    u64 ttbr, tcr, mair, sctlr;
    __asm__ volatile("mrs %0, ttbr0_el1; mrs %1, tcr_el1; mrs %2, mair_el1; mrs %3, sctlr_el1" : "=r"(ttbr), "=r"(tcr), "=r"(mair), "=r"(sctlr));
    int listed = 0, started = 0, slots = 0;
    int good[MAXCPU];
    boot_mask = gic_my_mask();
    for (int c = fdt_find_compatible(-1, "arm,cortex-a53"); c >= 0 && listed < 16; c = fdt_find_compatible(c, "arm,cortex-a53")) {
        const u8 *r = fdt_prop(c, "reg", &len);
        if (!r || (len != 4 && len != 8)) continue;
        u64 mpidr = 0;
        for (int i = 0; i < len; i++) mpidr = mpidr << 8 | r[i];
        listed++;
        if (mpidr == me) continue;
        if (slots + 1 >= MAXCPU) break;
        int idx = slots + 1;                                  /* a core PSCI started keeps its slot, answering or not */
        u8 *stack = kalloc(16384);
        u8 *exc = kalloc(32768);
        hc[idx].exc_top = (u8 *)(((usize)exc + 32768) & ~(usize)15);
        ctx[idx] = (struct ctx){ (u64)(usize)(stack + 16384), ttbr, tcr, mair, sctlr, (u64)(usize)vectors,
                                 (u64)(usize)secondary_main, (u64)idx };
        clean(&ctx[idx], sizeof ctx[idx]);
        clean((const void *)&alive[idx], 4);
        long rc = psci_call(0xc4000003, mpidr, (u64)(usize)secondary_entry, (u64)(usize)&ctx[idx]);   /* CPU_ON (64-bit) */
        if (rc) { klog("smp: core %llx did not start (PSCI %ld)", mpidr, rc); continue; }
        slots++;
        u64 t0 = k_now_ms();
        while (!alive[idx] && k_now_ms() - t0 < 1500) __asm__ volatile("yield");
        if (!alive[idx]) { klog("smp: core %llx started but did not answer within 1.5 s - it stays parked", mpidr); go[idx] = 2; continue; }
        mp[idx] = mpidr;
        good[started++] = idx;
    }
    /* the signals between cores, both ways: without them a program would wait forever
     * (interrupts masked here: the boot core looks at its pending signal itself) */
    u64 fm = irq_save();
    int ok = 0;
    for (int i = 0; i < started; i++) {
        int idx = good[i];
        u64 t0 = k_now_ms();
        while (!speed[idx] && k_now_ms() - t0 < 1000) __asm__ volatile("yield");
        W32(gic_dist() + 0xf10, 0xffu << 16);                   /* GICD_CPENDSGIR: SGI 2, from anyone - cleared first */
        gic_sgi(hc[idx].gicmask, 3);
        t0 = k_now_ms();
        while (!sgi_seen[idx] && k_now_ms() - t0 < 200) { gic_sgi(hc[idx].gicmask, 3); for (int k = 0; k < 2000; k++) __asm__ volatile("yield"); }
        int back = 0;
        t0 = k_now_ms();
        while (sgi_seen[idx] && !back && k_now_ms() - t0 < 200) back = (R32(gic_dist() + 0xf20) >> 16) & 0xff;   /* GICD_SPENDSGIR: SGI 2 pending here */
        if (!sgi_seen[idx] || !back) {
            klog("smp: core %llx: %s - it stays parked", mp[idx], !sgi_seen[idx] ? "the boot core's signal does not reach it" : "its signal does not reach the boot core");
            go[idx] = 2;
            continue;
        }
        W32(gic_dist() + 0xf10, 0xffu << 16);                   /* the test's SGI 2: not pending any more */
        good[ok++] = idx;
    }
    irq_restore(fm);
    __asm__ volatile("dsb sy" ::: "memory");
    /* only cores about as fast as this one take drawing work; all of them run programs */
    u64 f = irq_save();
    u64 mine = time_loop();                                           /* not interrupted */
    irq_restore(f);
    u64 best = mine;                                                  /* against the fastest core */
    for (int i = 0; i < ok; i++) if (speed[good[i]] && speed[good[i]] < best) best = speed[good[i]];
    char line[200] = "";
    int fast = 0;
    fmt(line, sizeof line, " boot:%llu%%", best * 100 / mine);
    for (int i = 0; i < ok; i++) {
        int idx = good[i];
        u64 sp = speed[idx];
        usize l = strlen(line);
        fmt(line + l, sizeof line - l, " %llx:%llu%%", mp[idx], sp ? best * 100 / sp : 0);
        if (sp && sp * 10 <= best * 13) { use[idx] = 1; fast++; }    /* within 30 % of the fastest */
    }
    nworkers = fast;
    for (int i = 0; i < ok; i++) __atomic_store_n(&go[good[i]], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&nhelpers, ok, __ATOMIC_RELEASE);
    __asm__ volatile("dsb sy; sev" ::: "memory");
    klog("smp: %d core(s) in the tree, %d started, %d answer signals; speed against the fastest:%s", listed ? listed : 1, started, ok, line);
    klog("smp: %d run programs, %d of them help with drawing", ok, fast);
    return fast;
}
