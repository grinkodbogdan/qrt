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
 * board instead of being evaluated from _CRS.  Each pad's PADCTRL0 bit 0 is
 * the input level (Linux pinctrl-cherryview.c documents the layout: pads in
 * families of 15, 0x400 apart, 8 bytes each, from offset 0x4400).
 *
 * The driver only reads the pads.  It samples every line at start-up and
 * treats a change from that resting level as a press, so it does not depend
 * on the (undocumented) polarity; a key held during boot starts QRT in
 * firmware mode anyway.  Polled from the input path at the frame rate, with
 * two-sample debouncing.
 *
 * Events: volume keys fire on press and repeat while held; the Windows
 * button fires on press; power fires SCAN_POWER when released within a
 * second, or SCAN_POWER_LONG once it has been held for a second.
 */
#include "buttons.h"

static const u64 community[] = { 0xfed80000, 0xfed88000, 0xfed90000, 0xfed98000 };   /* SW, N, E, SE */

static struct {
    const char *name;
    int comm, pin;
    u16 scan;
    int idle, last, stable, presses;
    u64 down_ms, next_ms;          /* press time; next repeat or long-press time */
    int fired;                     /* power: the long press was already reported */
} btn[] = {
    { "power",       2, 0x08, SCAN_POWER },
    { "volume up",   0, 0x5d, SCAN_VOLUP },
    { "volume down", 1, 0x08, SCAN_VOLDN },
    { "home",        0, 0x5f, SCAN_HOMEBTN },
};
#define N_BTN ((int)ARRAY_LEN(btn))

static int active;

int buttons_active(void) { return active; }

static volatile u32 *padctrl0(int comm, int pin) {
    return (volatile u32 *)(usize)(community[comm] + 0x4400 + 0x400 * (u64)(pin / 15) + 8 * (u64)(pin % 15));
}

static int level(int i) { return (int)(*padctrl0(btn[i].comm, btn[i].pin) & 1); }

int buttons_init(void) {
    if (!k.native || !k.is_venue) return 0;
    /* a missing community reads as all ones */
    for (usize c = 0; c < ARRAY_LEN(community); c++)
        if (*(volatile u32 *)(usize)(community[c] + 0x4400) == 0xffffffffu) { klog("buttons: GPIO community %u absent", (u32)c); return 0; }
    for (int i = 0; i < N_BTN; i++) {
        u32 v = *padctrl0(btn[i].comm, btn[i].pin);
        btn[i].idle = btn[i].last = btn[i].stable = (int)(v & 1);
        klog("buttons: %s = GPO%d pin 0x%x, PADCTRL0 %08x, resting level %d", btn[i].name, btn[i].comm, btn[i].pin, v, btn[i].idle);
    }
    active = 1;
    return 1;
}

int buttons_poll(event_t *out, int max) {
    if (!active) return 0;
    int n = 0;
    u64 now = k_now_ms();
    for (int i = 0; i < N_BTN && n < max; i++) {
        int l = level(i);
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
    return n;
}

void buttons_status(char *buf, usize cap) {
    if (!active) { strlcpy(buf, "not active (native mode on the Venue 8 Pro 5855 only)", cap); return; }
    usize o = 0;
    for (int i = 0; i < N_BTN; i++)
        o += fmt(buf + o, cap - o, "%s%s %d", i ? ", " : "polled GPIO; presses: ", btn[i].name, btn[i].presses);
}
