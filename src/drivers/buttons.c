/*
 * buttons.c - power and volume buttons of the Dell Venue 8 Pro 5855.
 *
 * On this tablet the buttons are plain Cherry Trail GPIO pads.  The DSDT
 * describes them twice, as a PNP0C40 "5 button array" (\_SB.TBAD) and as an
 * ACPI0011 generic-buttons device (\_SB.BTNS), whose _DSD gives each line
 * its HID usage:
 *
 *   GPO2 (east)      pin 0x08   usage 01:81  System Power Down  -> power
 *   GPO0 (southwest) pin 0x5D   usage 0C:E9  Volume Increment
 *   GPO1 (north)     pin 0x08   usage 0C:EA  Volume Decrement
 *   GPO0 (southwest) pin 0x5F   usage 07:E3  Left GUI (Windows button, if fitted)
 *
 * QRT has no AML interpreter yet, so the table is written down here for this
 * board instead of being evaluated from _CRS.  _CRS picks one of several pin
 * sets from the board id (GNVS BDID) and the PMIC id (GNVS PMID); the
 * driver reads both and follows the same choice:
 *   BDID 1          Windows button GPO1/0x08, volume down GPO0/0x3D
 *   PMID 3          Windows button on the PMIC (not supported here)
 *   otherwise       the table below
 *
 * The Button test also runs a pad scanner: it samples the input bit of every
 * pad in the four GPIO communities and lists the ones that changed, so a
 * press shows which pad really carries a button even if the tables are wrong.  Each pad's PADCTRL0 bit 0 is
 * the input level (Linux pinctrl-cherryview.c documents the layout: pads in
 * families of 15, 0x400 apart, 8 bytes each, from offset 0x4400).
 *
 * At start-up the driver puts each pad in GPIO-input mode, as Linux does
 * when gpio-keys requests the line (pinctrl-cherryview.c,
 * chv_gpio_request_enable + direction input): the firmware may leave a pad
 * in its native function, where the input bit does not follow the button.
 * It then samples every line and treats a change from that resting level as
 * a press, so it does not depend on the polarity.  It runs in both kernel
 * modes: the firmware does not read these pads after boot.  Polled from the input path at the frame rate, with
 * two-sample debouncing.
 *
 * Two more sources back the pad's input bit, for pads that do not show it:
 *   - the GPIO controller's interrupt-status register: the firmware gives
 *     each button pad an interrupt line (both edges); the status bit latches
 *     every edge even with the line masked, so polling it catches presses;
 *   - for power, the ACPI fixed power button: this FADT says the power
 *     button is fixed hardware (PWR_BUTTON = 0), so a press sets PWRBTN_STS
 *     in PM1_STS (I/O port 0x400 here), which needs no GPIO at all.
 * buttons_debug() shows all of it live (System Monitor, "Button test").
 *
 * Events: volume keys fire on press and repeat while held; the Windows
 * button fires on press; power fires SCAN_POWER when released within a
 * second, or SCAN_POWER_LONG once it has been held for a second.
 */
#include "buttons.h"
#include "touch.h"

static const u64 community[] = { 0xfed80000, 0xfed88000, 0xfed90000, 0xfed98000 };   /* SW, N, E, SE */

static struct {
    const char *name;
    int comm, pin;
    u16 scan;
    int idle, last, stable, presses;
    u64 down_ms, next_ms;          /* press time; next repeat or long-press time */
    int fired;                     /* power: the long press was already reported */
    int intsel;                    /* interrupt line of the pad, -1 = none */
    int edges, rx_moved, virt;     /* latched edges; the input bit ever changed; edge-tracked state */
} btn[] = {
    { "power",       2, 0x08, SCAN_POWER },
    { "volume up",   0, 0x5d, SCAN_VOLUP },
    { "volume down", 1, 0x08, SCAN_VOLDN },
    { "home",        0, 0x5f, SCAN_HOMEBTN },
};
#define N_BTN ((int)ARRAY_LEN(btn))

static int active, board_id = -1, pmic_id = -1;
static char why[96] = "not started: the firmware lists no button device (ACPI0011 / PNP0C40)";

/* ACPI fixed power button */
static struct { u16 sts, cnt; int on, presses, sci; } fx;
static inline u16 io_inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void io_outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
#define PWRBTN_STS (1u << 8)

static void fixed_button_init(void) {
    const u8 *f = k.fadt;
    if (!f || *(const u32 *)(f + 4) < 116) return;
    u32 flags = *(const u32 *)(f + 112), evt = *(const u32 *)(f + 56), cnt = *(const u32 *)(f + 64);
    if ((flags & (1u << 4)) || !evt || evt > 0xffff) return;      /* PWR_BUTTON set: a control-method button */
    fx.sts = (u16)evt; fx.cnt = (u16)cnt;
    /* ACPI mode: until SCI_EN is set the firmware's SMI handler owns the button */
    if (k.native && fx.cnt && !(io_inw(fx.cnt) & 1)) {
        u32 smi = *(const u32 *)(f + 48);
        u8 en = f[52];
        if (smi && en && smi <= 0xffff) {
            io_outb((u16)smi, en);
            for (int i = 0; i < 300 && !(io_inw(fx.cnt) & 1); i++) hal_delay_us(1000);
            klog("buttons: ACPI mode %s", io_inw(fx.cnt) & 1 ? "enabled" : "did not come on");
        }
    }
    fx.sci = fx.cnt ? io_inw(fx.cnt) & 1 : 0;
    io_outw(fx.sts, PWRBTN_STS);                                  /* clear a stale press */
    fx.on = 1;
    klog("buttons: ACPI fixed power button at port 0x%x (SCI_EN %d)", fx.sts, fx.sci);
}

int buttons_active(void) { return active; }

/* PADCTRL0/1 (Linux: CHV_PADCTRL0_*, CHV_PADCTRL1_*) */
#define PAD_GPIOEN      (1u << 15)
#define PAD_CFG_SHIFT   8
#define PAD_CFG_MASK    (7u << PAD_CFG_SHIFT)
#define PAD_CFG_GPO     1u
#define PAD_CFG_GPI     2u
#define PAD_CFG_HIZ     3u
#define PAD_RXSTATE     1u
#define PAD_TXSTATE     2u
#define PAD1_CFGLOCK    (1u << 31)

static volatile u32 *padctrl0(int comm, int pin) {
    return (volatile u32 *)(usize)(community[comm] + 0x4400 + 0x400 * (u64)(pin / 15) + 8 * (u64)(pin % 15));
}

static int level(int i) { return (int)(*padctrl0(btn[i].comm, btn[i].pin) & PAD_RXSTATE); }

int buttons_init(void) {
    if (active) return 1;
    if (!k.dsdt) { strlcpy(why, "no DSDT", sizeof why); return 0; }
    if (!venue_gnvs()) { fmt(why, sizeof why, "not the Venue 8 Pro 5855 firmware (DSDT %.6s)", (const char *)k.dsdt + 16); return 0; }
    const volatile u8 *gn = (const volatile u8 *)(usize)venue_gnvs();
    board_id = gn[0x322];                             /* BDID */
    pmic_id = gn[0x349];                              /* PMID */
    if (board_id == 1) {                              /* _CRS "PBUF" */
        btn[2].comm = 0; btn[2].pin = 0x3d;
        btn[3].comm = 1; btn[3].pin = 0x08;
    } else if (pmic_id == 3 && board_id != 9 && board_id != 10) {
        btn[3].pin = -1;                              /* "WBUF": the Windows button is on the PMIC */
    }
    klog("buttons: board id %d, PMIC id %d", board_id, pmic_id);
    for (int i = 0; i < N_BTN; i++) {
        if (btn[i].pin < 0) continue;
        volatile u32 *c0 = padctrl0(btn[i].comm, btn[i].pin), *c1 = c0 + 1;
        u32 v = *c0, v1 = *c1;
        if (v == 0xffffffffu) { fmt(why, sizeof why, "GPIO bank GPO%d does not answer", btn[i].comm); klog("buttons: %s", why); return 0; }
        u32 cfg = (v & PAD_CFG_MASK) >> PAD_CFG_SHIFT;
        if (!(v1 & PAD1_CFGLOCK) && (!(v & PAD_GPIOEN) || cfg == PAD_CFG_GPO || cfg == PAD_CFG_HIZ)) {
            *c0 = (v & ~PAD_CFG_MASK) | PAD_GPIOEN | (PAD_CFG_GPI << PAD_CFG_SHIFT);
            klog("buttons: %s pad switched to GPIO input (PADCTRL0 %08x -> %08x)", btn[i].name, v, *c0);
        }
        /* latch both edges in the interrupt status (the line stays masked) */
        btn[i].intsel = (int)((*c0 >> 28) & 15);
        if (!(*c1 & PAD1_CFGLOCK) && (*c1 & 7) == 0) *c1 = (*c1 & ~7u) | 3u;
    }
    hal_delay_us(2000);                               /* let the input settle */
    for (int i = 0; i < N_BTN; i++) {
        if (btn[i].pin < 0) continue;
        u32 v = *padctrl0(btn[i].comm, btn[i].pin);
        btn[i].idle = btn[i].last = btn[i].stable = (int)(v & PAD_RXSTATE);
        klog("buttons: %s = GPO%d pin 0x%x, PADCTRL0 %08x, resting level %d", btn[i].name, btn[i].comm, btn[i].pin, v, btn[i].idle);
    }
    fixed_button_init();
    active = 1;
    strlcpy(why, "active", sizeof why);
    return 1;
}

static volatile u32 *intstat(int comm) { return (volatile u32 *)(usize)(community[comm] + 0x300); }

/* ---- the pad scanner (Button test) ------------------------------------------------ */
static const int families[4] = { 7, 5, 2, 7 };       /* pads per community = families * 15 */
#define MAX_PADS 105
static struct {
    int on;
    u8 prev[4][MAX_PADS], valid[4][MAX_PADS];
    u16 hits[4][MAX_PADS];
    u16 ihits[4][16];
} sc;

void buttons_scan_start(void) {
    if (!venue_gnvs()) return;
    memset(&sc, 0, sizeof sc);
    for (int c = 0; c < 4; c++) {
        for (int p = 0; p < families[c] * 15; p++) {
            u32 v = *padctrl0(c, p);
            sc.valid[c][p] = v != 0xffffffffu && v != 0;
            sc.prev[c][p] = (u8)(v & PAD_RXSTATE);
        }
    }
    sc.on = 1;
}

static void scan_poll(void) {
    for (int c = 0; c < 4; c++) {
        for (int p = 0; p < families[c] * 15; p++) {
            if (!sc.valid[c][p]) continue;
            u8 v = (u8)(*padctrl0(c, p) & PAD_RXSTATE);
            if (v != sc.prev[c][p]) { sc.prev[c][p] = v; if (sc.hits[c][p] < 999) sc.hits[c][p]++; }
        }
        u32 is = *intstat(c) & 0xffff;
        for (int l = 0; l < 16; l++) if ((is >> l) & 1) { if (sc.ihits[c][l] < 999) sc.ihits[c][l]++; }
    }
}

static int scan_lines(char lines[][112], int n, int max) {
    static const char *cname[4] = { "SW", "N", "E", "SE" };
    if (!sc.on || n >= max) return n;
    char *l = lines[n++];
    usize o = fmt(l, 112, "pads that changed:");
    int any = 0;
    for (int c = 0; c < 4; c++)
        for (int p = 0; p < families[c] * 15; p++)
            if (sc.hits[c][p] && o < 100) { o += fmt(l + o, 112 - o, " %s/%02x x%u", cname[c], p, sc.hits[c][p]); any = 1; }
    if (!any) strlcpy(l + o, " none yet - press each button", 112 - o);
    if (n < max) {
        l = lines[n++];
        o = fmt(l, 112, "interrupt status lines seen:");
        any = 0;
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < 16; i++)
                if (sc.ihits[c][i] && o < 100) { o += fmt(l + o, 112 - o, " %s#%d", cname[c], i); any = 1; }
        if (!any) strlcpy(l + o, " none", 112 - o);
    }
    return n;
}

int buttons_poll(event_t *out, int max) {
    if (sc.on) scan_poll();
    if (!active) return 0;
    int n = 0;
    u64 now = k_now_ms();
    u32 ist[4];
    for (int c = 0; c < 4; c++) ist[c] = *intstat(c);
    for (int i = 0; i < N_BTN && n < max; i++) {
        if (btn[i].pin < 0) continue;
        int rx = level(i);
        if (rx != btn[i].idle) btn[i].rx_moved = 1;
        if (btn[i].intsel >= 0 && (ist[btn[i].comm] >> btn[i].intsel) & 1) {
            *intstat(btn[i].comm) = 1u << btn[i].intsel;               /* write 1 to clear */
            btn[i].edges++;
            if (!btn[i].rx_moved) btn[i].virt = !btn[i].virt;         /* the input bit is not moving: edges are the state */
        }
        int l = rx;
        if (!btn[i].rx_moved && btn[i].edges) { l = btn[i].virt ? !btn[i].idle : btn[i].idle; btn[i].last = l; }
        u16 sc = btn[i].scan;
        if (l == btn[i].last && l != btn[i].stable) {      /* the same new level twice in a row */
            btn[i].stable = l;
            if (l != btn[i].idle) {                          /* pressed */
                btn[i].presses++;
                btn[i].down_ms = now;
                btn[i].fired = 0;
                klog("buttons: %s pressed", btn[i].name);
                if (sc == SCAN_POWER) btn[i].next_ms = now + 1000;
                else {
                    btn[i].next_ms = now + 450;               /* first repeat */
                    out[n++] = (event_t){ .type = EV_KEY, .scan = sc };
                }
            } else if (sc == SCAN_POWER && !btn[i].fired) {   /* released before the long-press time */
                out[n++] = (event_t){ .type = EV_KEY, .scan = SCAN_POWER };
            }
        } else if (btn[i].stable != btn[i].idle && now >= btn[i].next_ms) {   /* held */
            if (sc == SCAN_POWER && !btn[i].fired) {
                btn[i].fired = 1;
                out[n++] = (event_t){ .type = EV_KEY, .scan = SCAN_POWER_LONG };
            } else if (sc == SCAN_VOLUP || sc == SCAN_VOLDN) {
                btn[i].next_ms = now + 120;
                out[n++] = (event_t){ .type = EV_KEY, .scan = sc };
            }
        }
        btn[i].last = l;
    }
    /* the ACPI fixed power button: a press, no release - a short press, unless the pad works */
    if (fx.on && n < max && (io_inw(fx.sts) & PWRBTN_STS)) {
        io_outw(fx.sts, PWRBTN_STS);
        fx.presses++;
        klog("buttons: ACPI power button");
        if (!btn[0].presses) out[n++] = (event_t){ .type = EV_KEY, .scan = SCAN_POWER };
    }
    return n;
}

/* the live state, one line per source, for the Button test screen */
int buttons_debug(char lines[][112], int max) {
    int n = 0;
    if (n < max) fmt(lines[n++], 112, "driver: %s (board id %d, PMIC id %d)", why, board_id, pmic_id);
    if (!active) return scan_lines(lines, n, max);
    for (int i = 0; i < N_BTN && n < max; i++) {
        if (btn[i].pin < 0) { fmt(lines[n++], 112, "%-11s on the PMIC (not supported)", btn[i].name); continue; }
        volatile u32 *c0 = padctrl0(btn[i].comm, btn[i].pin);
        u32 v = *c0, v1 = c0[1];
        fmt(lines[n++], 112, "%-11s GPO%d/%02x pad %08x %08x in=%u rest=%d gpio=%u cfg=%u lock=%u int=%d edges=%d presses=%d",
            btn[i].name, btn[i].comm, btn[i].pin, v, v1, v & 1, btn[i].idle, (v >> 15) & 1, (v >> 8) & 7, v1 >> 31,
            btn[i].intsel, btn[i].edges, btn[i].presses);
    }
    if (n < max) {
        if (fx.on) fmt(lines[n++], 112, "ACPI power button: PM1_STS %04x (port %x), SCI_EN %d, presses %d", io_inw(fx.sts), fx.sts, fx.sci, fx.presses);
        else strlcpy(lines[n++], "ACPI power button: not present", 112);
    }
    return scan_lines(lines, n, max);
}

void buttons_status(char *buf, usize cap) {
    if (!active) { strlcpy(buf, why, cap); return; }
    usize o = 0;
    for (int i = 0; i < N_BTN; i++)
        if (btn[i].pin >= 0)
            o += fmt(buf + o, cap - o, "%s%s %s, %d", i ? "; " : "", btn[i].name, level(i) != btn[i].idle ? "down" : "up", btn[i].presses);
}
