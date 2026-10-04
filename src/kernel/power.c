/*
 * power.c - turning the machine off and restarting it, properly.
 *
 * First the programs: each is asked to stop (SIGTERM, if it handles it), and whatever
 * is still running after 3 s is stopped.  Then the devices go quiet: Bluetooth audio
 * and Wi-Fi disconnect, the backlight goes off.  Then the machine:
 *
 *   off      ACPI sleep state S5, as Linux does: the SLP_TYP values come from the
 *            \_S5 package in the DSDT (or an SSDT), written to the FADT's PM1 control
 *            registers, or to its sleep control register on "hardware-reduced" ACPI
 *            machines (Cherry Trail tablets).  If the machine is still on a second
 *            later, the firmware's ResetSystem(Shutdown).
 *   restart  the firmware's ResetSystem(Cold); then the FADT's reset register; then
 *            the chipset's reset control (port 0xCF9); then the keyboard controller.
 */
#include "kernel.h"
#include "../drivers/backlight.h"
#include "../net/wlan.h"
#include "../drivers/bt/bt.h"
#if defined(__x86_64__)
#include "../arch/x64/proc.h"
#endif

static inline void p_outb(u16 port, u8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void p_outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void p_outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }

/* ---- ACPI ---------------------------------------------------------------------------------- */
/* a Generic Address Structure: space 0 = memory, 1 = I/O ports; width from the access size or bit width */
static int gas_write(const u8 *g, u32 v, int bytes) {
    u8 space = g[0];
    u64 addr = *(const u64 *)(g + 4);
    if (!addr) return 0;
    if (g[3]) bytes = 1 << (g[3] - 1);                       /* access size: 1 byte, 2, 4, 8 */
    if (space == 1) {
        if (bytes == 1) p_outb((u16)addr, (u8)v);
        else if (bytes == 2) p_outw((u16)addr, (u16)v);
        else p_outl((u16)addr, v);
        return 1;
    }
    if (space == 0) {
        if (sizeof(void *) == 4 && addr >> 32) return 0;
        volatile void *p = (volatile void *)(usize)addr;
        if (bytes == 1) *(volatile u8 *)p = (u8)v;
        else if (bytes == 2) *(volatile u16 *)p = (u16)v;
        else *(volatile u32 *)p = v;
        return 1;
    }
    return 0;
}

/* \_S5's SLP_TYPa and SLP_TYPb, read straight from the AML bytes: Name(_S5_, Package(){a, b, ...}) */
static int find_s5(const u8 *t, u8 *a, u8 *b) {
    if (!t) return 0;
    u32 len = *(const u32 *)(t + 4);
    for (u32 i = 36; i + 8 < len; i++) {
        if (memcmp(t + i, "_S5_", 4) || t[i + 4] != 0x12) continue;          /* PackageOp */
        if (!(t[i - 1] == 0x08 || (t[i - 2] == 0x08 && t[i - 1] == '\\'))) continue;   /* NameOp (maybe \_S5_) */
        const u8 *p = t + i + 5;
        p += ((*p & 0xC0) >> 6) + 2;                         /* the package length, then NumElements */
        u8 v[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A) { v[k] = p[1]; p += 2; }         /* BytePrefix */
            else if (*p == 0x00 || *p == 0x01) { v[k] = *p; p++; }   /* ZeroOp, OneOp */
            else if (*p == 0xFF) { v[k] = 0xFF; p++; }       /* OnesOp */
            else return 0;
        }
        *a = v[0] & 7; *b = v[1] & 7;
        return 1;
    }
    return 0;
}

static void acpi_off(void) {
    const u8 *f = acpi_table("FACP", 0);
    if (!f) { klog("power: no FADT"); return; }
    u32 flen = *(const u32 *)(f + 4);
    const u8 *dsdt = NULL;
    if (flen >= 148 && *(const u64 *)(f + 140) && !(sizeof(void *) == 4 && *(const u64 *)(f + 140) >> 32))
        dsdt = (const u8 *)(usize)*(const u64 *)(f + 140);
    else if (*(const u32 *)(f + 40)) dsdt = (const u8 *)(usize)*(const u32 *)(f + 40);
    u8 a = 0, b = 0;
    int found = find_s5(dsdt, &a, &b);
    for (int i = 0; !found && i < 32; i++) {
        const u8 *s = acpi_table("SSDT", i);
        if (!s) break;
        found = find_s5(s, &a, &b);
    }
    if (!found) { klog("power: no \\_S5 in the ACPI tables"); return; }
    u32 flags = flen >= 116 ? *(const u32 *)(f + 112) : 0;
    klog("power: ACPI S5 (SLP_TYP %u/%u%s)", a, b, (flags >> 20) & 1 ? ", hardware-reduced" : "");
    __asm__ volatile("cli");
    if (flen >= 256 && *(const u64 *)(f + 244 + 4)) {       /* SLEEP_CONTROL_REG (ACPI 5) */
        gas_write(f + 244, (u32)(a << 2) | (1u << 5), 1);
        return;
    }
    /* PM1a/PM1b control: SLP_TYP in bits 10-12, SLP_EN bit 13 */
    u32 va = ((u32)a << 10) | (1u << 13), vb = ((u32)b << 10) | (1u << 13);
    if (flen >= 196 && *(const u64 *)(f + 172 + 4)) gas_write(f + 172, va, 2);   /* X_PM1a_CNT_BLK */
    else if (*(const u32 *)(f + 64)) p_outw((u16)*(const u32 *)(f + 64), (u16)va);
    if (flen >= 208 && *(const u64 *)(f + 184 + 4)) gas_write(f + 184, vb, 2);   /* X_PM1b_CNT_BLK */
    else if (*(const u32 *)(f + 68)) p_outw((u16)*(const u32 *)(f + 68), (u16)vb);
}

static void acpi_reset(void) {
    const u8 *f = acpi_table("FACP", 0);
    if (!f || *(const u32 *)(f + 4) < 129) return;
    u32 flags = *(const u32 *)(f + 112);
    if (!((flags >> 10) & 1)) return;                        /* RESET_REG_SUP */
    klog("power: ACPI reset register");
    gas_write(f + 116, f[128], 1);
}

/* ---- programs and devices -------------------------------------------------------------------- */
static void wait_ms(u32 ms) {
    u64 end = k_now_ms() + ms;
    while (k_now_ms() < end) hal_wait_frame_ms(10);
}

static void stop_everything(const char *why) {
    klog("power: %s: stopping programs", why);
#if defined(__x86_64__)
    if (k.native) {
        int live = 0;
        for (int i = 0; proc_at(i); i++) {
            proc_t *p = proc_at(i);
            if (p->exited || p->exiting) continue;
            live++;
            if (p->sa[15].handler > 1) sig_post(p, NULL, 15, 0, 0, 0);   /* SIGTERM: a program that handles it saves and exits */
            else proc_kill(p);
        }
        u64 end = k_now_ms() + 3000;
        while (live && k_now_ms() < end) {
            hal_wait_frame_ms(20);
            live = 0;
            for (int i = 0; proc_at(i); i++) if (!proc_at(i)->exited) live++;
        }
        for (int i = 0; proc_at(i); i++) if (!proc_at(i)->exited) proc_kill(proc_at(i));
        if (live) wait_ms(500);
    }
#endif
    klog("power: devices off");
    bt_audio_disconnect();
    wlan_disconnect();
    backlight_power(0);
    wait_ms(300);                                            /* the radios' last frames go out */
}

void power_off(void) {
    stop_everything("shutting down");
    acpi_off();
    wait_ms(1000);                                           /* still here: the firmware's way */
    klog("power: firmware shutdown");
    k.rt->ResetSystem(EfiResetShutdown, 0, 0, NULL);
}

void power_restart(void) {
    stop_everything("restarting");
    klog("power: firmware restart");
    k.rt->ResetSystem(EfiResetCold, 0, 0, NULL);
    acpi_reset();
    wait_ms(500);
    p_outb(0xCF9, 0x02); p_outb(0xCF9, 0x06);                /* the chipset: full reset */
    wait_ms(500);
    p_outb(0x64, 0xFE);                                      /* the keyboard controller's reset line */
    for (;;) __asm__ volatile("hlt");
}
