/*
 * smp.c - multi-core work dispatch.
 *
 * UEFI's MP Services protocol is the firmware's own scheduler for the other
 * cores: StartupAllAPs() wakes every application processor, runs one
 * procedure on each, and returns when all are done.  Work items are claimed
 * through an atomic counter, so each AP takes the next unclaimed index, and
 * anything left over (an AP that was busy, disabled or timed out) is run by
 * the boot core afterwards.
 *
 * APs are started by firmware that is itself built without SSE, so a core
 * may come up with SSE disabled.  Every AP therefore enables the FPU and SSE
 * for itself (CR0/CR4, MXCSR) before touching compiled code that may use it.
 */
#include "smp.h"

static EFI_GUID mp_guid = MP_SERVICES_GUID;
static EFI_MP_SERVICES_PROTOCOL *mp;
static int workers, enabled = 1;

typedef struct {
    smp_job_t job;
    void *arg;
    int count;
    volatile int next;
    volatile u8 done[64];
} dispatch_t;

__attribute__((noinline)) static void ap_work(dispatch_t *d) {
    for (;;) {
        int i = __atomic_fetch_add(&d->next, 1, __ATOMIC_SEQ_CST);
        if (i >= d->count) break;
        d->job(d->arg, i, d->count);
        __atomic_store_n(&d->done[i], 1, __ATOMIC_RELEASE);
    }
}

static void ap_entry(void *p) {
    /* CR0: clear EM (bit 2), set MP (bit 1); CR4: OSFXSR (bit 9) + OSXMMEXCPT (bit 10) */
    usize cr;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr));
    cr = (cr & ~(usize)4) | 2;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr));
    cr |= (1u << 9) | (1u << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr));
    u32 mxcsr = 0x1f80;
    __asm__ volatile("fninit; ldmxcsr %0" : : "m"(mxcsr));
    ap_work((dispatch_t *)p);
}

static void run_on_aps(dispatch_t *d) {
    /* blocking call; 2 s timeout so a wedged core cannot hang the shell */
    mp->StartupAllAPs(mp, ap_entry, 0, NULL, 2000000, d, NULL);
}

/* Self-test: every worker must produce the right floating-point result. */
static float test_out[64];
static void test_job(void *arg, int i, int n) {
    float x = (float)(i + 1);
    test_out[i] = fsqrt(x * x * 4.0f) + fsin(0.0f);   /* = 2(i+1) */
}

int smp_init(void) {
    UINTN total = 0, en = 0;
    if (EFI_ERROR(k.bs->LocateProtocol(&mp_guid, NULL, (void **)&mp)) || !mp ||
        EFI_ERROR(mp->GetNumberOfProcessors(mp, &total, &en)) || en < 2) {
        klog("smp: single core (%u cpus reported)", (u32)total);
        mp = NULL;
        return 0;
    }
    workers = (int)MIN(en - 1, 63);
    enabled = (int)hal_setting_get(u"QrtSmp", 1) & 1;

    /* prove the APs run our code and do floating point correctly */
    dispatch_t d = { test_job, NULL, workers, 0, { 0 } };
    memset(test_out, 0, sizeof test_out);
    u64 t0 = k_now_us();
    run_on_aps(&d);
    u64 t1 = k_now_us();
    int ok = 0;
    for (int i = 0; i < workers; i++)
        if (d.done[i] && test_out[i] > 2.0f * (i + 1) - 0.01f && test_out[i] < 2.0f * (i + 1) + 0.01f) ok++;
    if (ok != workers) {
        klog("smp: self-test failed (%d of %d cores) - staying single core", ok, workers);
        workers = 0;
        mp = NULL;
        return 0;
    }
    klog("smp: %u cpus, %d worker cores ready (wake-up %llu us)%s", (u32)en, workers, t1 - t0,
         enabled ? "" : ", disabled in settings");
    return workers;
}

#if defined(__x86_64__)
int  native_smp_workers(void);
void native_smp_run(smp_job_t job, void *arg, int count);
#endif

int smp_workers(void) {
#if defined(__x86_64__)
    if (k.native) return enabled ? native_smp_workers() : 0;
#endif
    return mp && enabled ? workers : 0;
}
int  smp_enabled(void) { return enabled; }
void smp_set_enabled(int on) { enabled = on ? 1 : 0; hal_setting_set(u"QrtSmp", (u32)enabled); }

void smp_run(smp_job_t job, void *arg, int count) {
#if defined(__x86_64__)
    if (k.native) {
        if (smp_workers()) native_smp_run(job, arg, count);
        else for (int i = 0; i < count; i++) job(arg, i, count);
        return;
    }
#endif
    dispatch_t d = { job, arg, MIN(count, 64), 0, { 0 } };
    if (smp_workers() && d.count > 1) run_on_aps(&d);
    for (int i = 0; i < d.count; i++)           /* leftovers, or everything when single core */
        if (!d.done[i]) job(arg, i, d.count);
}
