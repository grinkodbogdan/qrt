/*
 * gic.c - interrupts on 64-bit ARM: the GICv2 and the generic timer.
 *
 * Tessera takes one interrupt: the virtual timer (PPI 27), every 10 ms - the scheduler's
 * tick (sched.c), so a thread that spins cannot keep the others from running.  The
 * device interrupts (SPIs) belong to Linux's drivers: Linux's qrt-gic irqchip enables,
 * masks and configures them in the distributor at a priority under the CPU interface's
 * mask, so they never interrupt the CPU; gic_pending() finds them pending and enabled
 * and linux.c hands them to Linux.
 */
#include "arm.h"
#include "sched.h"

static u64 dist, cpu;
static u32 lines, tick_cnt;
static u64 ticks;

u64 gic_dist(void) { return dist; }
u64 gic_ticks(void) { return ticks; }

static void timer_arm(void) {
    __asm__ volatile("msr cntv_tval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"((u64)tick_cnt), "r"(1ull));
}

int gic_init(void) {
    static const char *const compat[] = { "qcom,msm-qgic2", "arm,gic-400", "arm,cortex-a15-gic", "arm,cortex-a7-gic", NULL };
    int n = -1;
    for (int i = 0; compat[i] && n < 0; i++) n = fdt_find_compatible(-1, compat[i]);
    u64 s;
    if (n < 0 || !fdt_reg(n, 0, &dist, &s) || !fdt_reg(n, 1, &cpu, &s)) { klog("gic: none in the device tree: no preemption"); return 0; }
    lines = 32 * ((R32(dist + 0x004) & 0x1f) + 1);
    if (lines > 1020) lines = 1020;
    W32(dist + 0x180, ~0u);                                   /* SGIs and PPIs off ... */
    *(volatile u8 *)(usize)(dist + 0x400 + 27) = 0x80;        /* ... but the virtual timer, above the mask */
    W32(dist + 0x100, 1u << 27);
    W32(dist, 1);
    W32(cpu + 0x04, 0x90);                                    /* GICC_PMR: SPIs (0xa0) stay below it */
    W32(cpu, 1);
    u64 freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    tick_cnt = (u32)(freq / 100);
    timer_arm();
    klog("gic: distributor %llx, CPU interface %llx, %u lines; 10 ms scheduler tick", dist, cpu, lines);
    return 1;
}

/* the IRQ exception (main.c), interrupts masked */
void gic_irq(void) {
    for (;;) {
        u32 iar = R32(cpu + 0x0c), id = iar & 0x3ff;
        if (id >= 1020) return;
        W32(cpu + 0x10, iar);                                 /* EOI before switching away */
        if (id == 27) { timer_arm(); ticks++; thr_tick(); }
    }
}

/* SPIs pending and enabled, for Linux: fn(id) each; edge-triggered ones are cleared */
int gic_pending(void (*fn)(u32 id)) {
    int n = 0;
    for (u32 w = 1; dist && w < lines / 32; w++) {
        u32 p = R32(dist + 0x200 + 4 * w) & R32(dist + 0x100 + 4 * w);
        while (p) {
            u32 b = (u32)__builtin_ctz(p), id = w * 32 + b;
            p &= p - 1;
            if (R32(dist + 0xc00 + 4 * (id / 16)) & (2u << (2 * (id % 16)))) W32(dist + 0x280 + 4 * w, 1u << b);   /* edge: clear */
            fn(id);
            n++;
        }
    }
    return n;
}
