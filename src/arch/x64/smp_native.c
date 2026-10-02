/*
 * smp_native.c - start the other CPU cores ourselves and give them work.
 *
 * Cores come from the ACPI MADT.  Each is started with INIT + STARTUP IPIs
 * into trampoline.S, lands in ap_main() with its own stack, GDT/TSS and APIC,
 * and then sleeps (hlt) until the boot core broadcasts VEC_WAKE with a
 * render job.  The boot core works on the same job, so drawing uses every
 * core.  Workers never run threads, touch firmware or allocate memory.
 */
#include "arch.h"
#include "../../kernel/smp.h"

extern u8 tramp_start[], tramp_end[], tramp_gdtr_base[], tramp_jmp32[], tramp_jmp64[];
extern u8 tramp_cr3[], tramp_stack[], tramp_entry[], tramp_arg[], tramp_pm32[], tramp_lm64[], tramp_gdt[];
u64 native_trampoline(void);

static struct {
    smp_job_t job;
    void *arg;
    int count;
    volatile int next, done, active;
    volatile u64 gen;
} J;
static volatile int alive;
static int workers;

int native_smp_workers(void) { return workers; }

/* The job is handed over under a small lock: the boot core publishes a job only when no
 * worker is inside one, and a worker takes a consistent snapshot (job, argument, count)
 * and marks itself active in the same step.  Without it a worker that woke late could
 * read the next job's function with the last one's argument while it was being written. */
static volatile int jlock;
static void jl_lock(void) { while (__atomic_exchange_n(&jlock, 1, __ATOMIC_ACQUIRE)) pause(); }
static void jl_unlock(void) { __atomic_store_n(&jlock, 0, __ATOMIC_RELEASE); }

static void work(smp_job_t job, void *arg, int count) {
    for (;;) {
        int i = __atomic_fetch_add(&J.next, 1, __ATOMIC_SEQ_CST);
        if (i >= count) break;
        job(arg, i, count);
        __atomic_fetch_add(&J.done, 1, __ATOMIC_SEQ_CST);
    }
}

static void ap_main(void *arg) {
    percpu_t *c = arg;
    cpu_setup(c, c->index);
    lapic_init();
    c->apic_id = lapic_id();
    __atomic_fetch_add(&alive, 1, __ATOMIC_SEQ_CST);
    u64 seen = 0;
    for (;;) {
        cli();
        if (__atomic_load_n(&J.gen, __ATOMIC_ACQUIRE) == seen) { sti_hlt(); continue; }
        jl_lock();
        seen = J.gen;
        smp_job_t job = J.job;
        void *jarg = J.arg;
        int count = J.count;
        __atomic_fetch_add(&J.active, 1, __ATOMIC_SEQ_CST);
        jl_unlock();
        sti();
        work(job, jarg, count);
        __atomic_fetch_sub(&J.active, 1, __ATOMIC_SEQ_CST);
    }
}

void native_smp_run(smp_job_t job, void *arg, int count) {
    jl_lock();
    while (__atomic_load_n(&J.active, __ATOMIC_ACQUIRE)) { jl_unlock(); pause(); jl_lock(); }   /* no worker in an old job */
    J.job = job; J.arg = arg; J.count = count;
    J.next = 0; J.done = 0;
    __atomic_fetch_add(&J.gen, 1, __ATOMIC_RELEASE);
    jl_unlock();
    if (workers) lapic_broadcast_ipi(VEC_WAKE);
    work(job, arg, count);                            /* the boot core helps */
    while (__atomic_load_n(&J.done, __ATOMIC_ACQUIRE) < count || __atomic_load_n(&J.active, __ATOMIC_ACQUIRE))
        pause();
}

void native_smp_start(void) {
    u64 page = native_trampoline();
    const u8 *madt = acpi_table("APIC", 0);
    if (!page || !madt || kernel_cr3() >> 32) { klog("smp: no trampoline/MADT - single core"); return; }

    usize tlen = (usize)(tramp_end - tramp_start);
    u8 *t = (u8 *)(usize)page;
    memcpy(t, tramp_start, tlen);
#define OFF(sym) ((usize)((sym) - tramp_start))
    *(u32 *)(t + OFF(tramp_gdtr_base)) = (u32)(page + OFF(tramp_gdt));
    *(u32 *)(t + OFF(tramp_jmp32)) = (u32)(page + OFF(tramp_pm32));
    *(u32 *)(t + OFF(tramp_jmp64)) = (u32)(page + OFF(tramp_lm64));
    *(u64 *)(t + OFF(tramp_cr3)) = kernel_cr3();
    *(u64 *)(t + OFF(tramp_entry)) = (u64)(usize)ap_main;

    u32 me = lapic_id();
    u32 len = *(const u32 *)(madt + 4);
    for (u32 off = 44; off + 2 <= len && ncpus < MAX_CPUS; off += madt[off + 1]) {
        const u8 *e = madt + off;
        if (e[1] < 2) break;
        u32 id, flags;
        if (e[0] == 0 && e[1] >= 8) { id = e[3]; flags = *(const u32 *)(e + 4); }
        else if (e[0] == 9 && e[1] >= 16) { id = *(const u32 *)(e + 4); flags = *(const u32 *)(e + 8); }
        else continue;
        if (!(flags & 1) || id == me) continue;

        percpu_t *c = heap_alloc(sizeof *c);
        c->index = (u32)ncpus;
        u64 stack = pmm_alloc_contig(8);                    /* 32 KiB */
        *(u64 *)(t + OFF(tramp_stack)) = stack + 8 * PAGE;
        *(u64 *)(t + OFF(tramp_arg)) = (u64)(usize)c;
        int before = alive;
        lapic_init_sipi(id, (u32)(page >> 12));
        u64 deadline = k_now_ms() + 200;
        while (alive == before && k_now_ms() < deadline) pause();
        if (alive == before) { klog("smp: core with APIC id %u did not start", id); continue; }
        ncpus++;
        workers++;
    }
    klog("smp: %d cores online (boot core + %d workers), started without firmware help", ncpus, workers);
}
