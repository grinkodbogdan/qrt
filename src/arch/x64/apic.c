/*
 * apic.c - local APIC: timer interrupts, inter-processor interrupts.
 *
 * Works in xAPIC (MMIO at the APIC base) and x2APIC (MSRs) mode, whichever
 * the firmware left enabled.  The legacy 8259 PICs are masked; nothing in
 * QRT uses them.
 */
#include "arch.h"

enum { R_ID = 0x20, R_TPR = 0x80, R_EOI = 0xb0, R_SVR = 0xf0, R_ICR_LO = 0x300, R_ICR_HI = 0x310,
       R_LVT_TIMER = 0x320, R_TIMER_INIT = 0x380, R_TIMER_CUR = 0x390, R_TIMER_DIV = 0x3e0 };

static volatile u8 *mmio;
static int x2;
static u32 ticks_per_ms;
volatile u64 ticks;
u32 tick_hz;

void k_delay_us(u64 us) {
    u64 end = rdtsc64() + us * k.tsc_per_ms / 1000;
    while (rdtsc64() < end) pause();
}

static u32 rd(u32 reg) {
    if (x2) return (u32)rdmsr(0x800 + (reg >> 4));
    return *(volatile u32 *)(mmio + reg);
}

static void wr(u32 reg, u32 v) {
    if (x2) wrmsr(0x800 + (reg >> 4), v);
    else *(volatile u32 *)(mmio + reg) = v;
}

u32 lapic_id(void) { return x2 ? rd(R_ID) : rd(R_ID) >> 24; }
void lapic_eoi(void) { wr(R_EOI, 0); }

void lapic_init(void) {
    u64 base = rdmsr(MSR_APIC_BASE);
    x2 = (base >> 10) & 1;
    mmio = (volatile u8 *)(usize)(base & 0xfffff000ull);
    if (!(base & (1u << 11))) wrmsr(MSR_APIC_BASE, base | (1u << 11));   /* globally enabled */
    wr(R_TPR, 0);
    wr(R_SVR, 0x100 | VEC_SPURIOUS);
    static int pic_masked;
    if (!pic_masked) { outb(0x21, 0xff); outb(0xa1, 0xff); pic_masked = 1; }
}

/* another core's timer, at its own rate (the calibration is the boot core's) */
void lapic_timer_start_ap(u32 hz) {
    wr(R_TIMER_DIV, 0x3);
    wr(R_LVT_TIMER, VEC_TIMER | (1u << 17));
    wr(R_TIMER_INIT, ticks_per_ms * 1000 / hz);
}

void lapic_timer_start(u32 hz) {
    wr(R_TIMER_DIV, 0x3);                          /* divide by 16 */
    if (!ticks_per_ms) {
        /* calibrate against the TSC, which boot already timed with the firmware */
        wr(R_LVT_TIMER, 1u << 16);                 /* masked, one-shot */
        wr(R_TIMER_INIT, 0xffffffffu);
        k_delay_us(10000);
        ticks_per_ms = (0xffffffffu - rd(R_TIMER_CUR)) / 10;
        if (!ticks_per_ms) ticks_per_ms = 100000;
    }
    tick_hz = hz;                                  /* the handler is the scheduler's (sched.c) */
    wr(R_LVT_TIMER, VEC_TIMER | (1u << 17));       /* periodic */
    wr(R_TIMER_INIT, ticks_per_ms * 1000 / hz);
}

static void wait_icr(void) {
    if (x2) return;
    for (int i = 0; i < 1000000 && (rd(R_ICR_LO) & (1u << 12)); i++) pause();
}

static void send_icr(u32 dest, u32 lo) {
    if (x2) { wrmsr(0x830, ((u64)dest << 32) | lo); return; }
    /* two writes: an interrupt between them that sends its own IPI would retarget this one */
    u64 fl = irq_save();
    wait_icr();
    wr(R_ICR_HI, dest << 24);
    wr(R_ICR_LO, lo);
    wait_icr();
    irq_restore(fl);
}

void lapic_send_ipi(u32 apic_id, u32 vector) { send_icr(apic_id, vector); }
void lapic_broadcast_ipi(u32 vector) { send_icr(0, vector | (3u << 18)); }   /* all excluding self */

void lapic_init_sipi(u32 apic_id, u32 page) {
    send_icr(apic_id, 0x4500);                     /* INIT, level assert */
    k_delay_us(10000);
    for (int i = 0; i < 2; i++) {
        send_icr(apic_id, 0x4600 | (page & 0xff)); /* STARTUP at page * 4 KiB */
        k_delay_us(200);
    }
}
