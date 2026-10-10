/*
 * smp.c - the other CPU cores on 64-bit ARM.  Threads still run on the boot core (the
 * scheduler is single-core); the others are started through PSCI CPU_ON and do the
 * shell's pure pixel work (smp_run: the wallpaper, turning the picture, ...) - as the
 * x86-64 kernel's application processors first did.  A job must not call the kernel:
 * no heap, no log, no locks - only computation on memory.
 */
#include "arm.h"
#include "../../kernel/smp.h"

#define MAXCPU 8
extern char secondary_entry[], vectors[];
static struct ctx { u64 sp, ttbr, tcr, mair, sctlr, vbar, entry, idx; } __attribute__((aligned(64))) ctx[MAXCPU];
static volatile u32 alive[MAXCPU];
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

static void secondary_main(u64 idx) {
    alive[idx] = 1;
    __asm__ volatile("dsb sy; sev" ::: "memory");
    u32 seen = job.gen;
    for (;;) {
        while (__atomic_load_n(&job.gen, __ATOMIC_ACQUIRE) == seen) __asm__ volatile("wfe");
        seen = job.gen;
        run_indices();
    }
}

int smp_workers(void) { return nworkers; }
int smp_enabled(void) { return nworkers > 0; }
void smp_set_enabled(int on) { (void)on; }
int smp_init(void) { return nworkers; }

void smp_run(smp_job_t fn, void *arg, int count) {
    if (!nworkers || count < 2) { for (int i = 0; i < count; i++) fn(arg, i, count); return; }
    job.fn = fn; job.arg = arg; job.count = count;
    next_idx = 0; done_idx = 0;
    __atomic_fetch_add(&job.gen, 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb sy; sev" ::: "memory");
    run_indices();                               /* the boot core works too */
    while (__atomic_load_n(&done_idx, __ATOMIC_ACQUIRE) < count) __asm__ volatile("yield");
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
    int listed = 0, started = 0;
    for (int c = fdt_find_compatible(-1, "arm,cortex-a53"); c >= 0 && listed < MAXCPU; c = fdt_find_compatible(c, "arm,cortex-a53")) {
        const u8 *r = fdt_prop(c, "reg", &len);
        if (!r || (len != 4 && len != 8)) continue;
        u64 mpidr = 0;
        for (int i = 0; i < len; i++) mpidr = mpidr << 8 | r[i];
        listed++;
        if (mpidr == me) continue;
        int idx = started + 1;
        if (idx >= MAXCPU) break;
        u8 *stack = kalloc(16384);
        ctx[idx] = (struct ctx){ (u64)(usize)(stack + 16384), ttbr, tcr, mair, sctlr, (u64)(usize)vectors,
                                 (u64)(usize)secondary_main, (u64)idx };
        clean(&ctx[idx], sizeof ctx[idx]);
        clean((const void *)&alive[idx], 4);
        long rc = psci_call(0xc4000003, mpidr, (u64)(usize)secondary_entry, (u64)(usize)&ctx[idx]);   /* CPU_ON (64-bit) */
        if (rc) { klog("smp: core %llx did not start (PSCI %ld)", mpidr, rc); continue; }
        for (int t = 0; t < 100000 && !alive[idx]; t++) __asm__ volatile("isb");
        if (!alive[idx]) {
            u64 t0 = k_now_ms();
            while (!alive[idx] && k_now_ms() - t0 < 200) __asm__ volatile("yield");
        }
        if (!alive[idx]) { klog("smp: core %llx started but never answered", mpidr); continue; }
        started++;
        nworkers = started;                       /* usable from now on */
    }
    klog("smp: %d core(s) in the tree; this one runs the threads, %d help with drawing", listed ? listed : 1, started);
    return started;
}
