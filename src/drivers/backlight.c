/*
 * backlight.c - the Venue 8 Pro 5855's panel backlight.
 *
 * The panel is MIPI DSI and its backlight is driven by the SoC's LPSS PWM
 * controller #1 (ACPI 80862288).  Under Windows the graphics driver sets
 * the level; the DSDT shows the link: \_SB.PCI0.GFX0._PS3/_PS0 save and
 * restore that PWM's control register (PWMC, at the controller's BAR),
 * and the base address comes from the firmware NVS variable P10A.
 *
 * The PWM control register (Linux pwm-lpss.c):
 *   bit 31  enable
 *   bit 30  software update (latch base unit and on-time; self-clearing)
 *   8..23   base unit (frequency)
 *   0..7    on-time divisor: output high for (255 - div) / 256 of a period
 *
 * QRT keeps the firmware's frequency and changes only the on-time.  It
 * takes control only if the firmware left the PWM enabled, so it never
 * touches a PWM that drives something else.  Native mode only: in firmware
 * mode the GOP driver may own it.
 */
#include "backlight.h"
#include "touch.h"

#define GNVS_P10A 376          /* PWM #1 MMIO base (DSDT field offset) */
#define PWM_ENABLE (1u << 31)
#define PWM_UPDATE (1u << 30)

static struct { volatile u32 *ctrl; u32 saved; int level, on; } bl;

static void write_level(int pct) {
    u32 div = (u32)(255 - (255 * pct + 50) / 100);
    u32 v = (bl.saved & ~0xffu & ~PWM_UPDATE) | PWM_ENABLE | div;
    *bl.ctrl = v;
    *bl.ctrl = v | PWM_UPDATE;
}

int backlight_init(void) {
    if (!k.native || !k.is_venue || bl.ctrl) return bl.ctrl != NULL;
    u32 g = venue_gnvs();
    if (!g) return 0;
    u32 base = *(volatile u32 *)(usize)(g + GNVS_P10A);
    if (!base || base == 0xffffffffu) { klog("backlight: no PWM base in NVS"); return 0; }
    volatile u32 *ctrl = (volatile u32 *)(usize)base;
    u32 v = *ctrl;
    klog("backlight: PWM #1 at %08x, control %08x", base, v);
    if (v == 0xffffffffu || !(v & PWM_ENABLE)) { klog("backlight: PWM not running; left alone"); return 0; }
    bl.ctrl = ctrl;
    bl.saved = v & ~PWM_UPDATE;
    int cur = (int)((255 - (v & 0xff)) * 100 / 255);
    bl.level = (int)hal_setting_get(u"QrtBrightness", (u32)CLAMP(cur, 5, 100));
    bl.level = CLAMP(bl.level, 5, 100);
    bl.on = 1;
    write_level(bl.level);
    return 1;
}

int backlight_available(void) { return bl.ctrl != NULL; }
int backlight_level(void) { return bl.level; }

void backlight_set_level(int pct) {
    if (!bl.ctrl) return;
    bl.level = CLAMP(pct, 5, 100);
    if (bl.on) write_level(bl.level);
    hal_setting_set(u"QrtBrightness", (u32)bl.level);
}

void backlight_power(int on) {
    if (!bl.ctrl || bl.on == on) return;
    bl.on = on;
    if (on) write_level(bl.level);
    else { write_level(0); *bl.ctrl = (*bl.ctrl & ~PWM_ENABLE) | PWM_UPDATE; }
}

void backlight_status(char *buf, usize cap) {
    if (!bl.ctrl) { strlcpy(buf, k.native ? "PWM not running (not the backlight here)" : "left to the firmware", cap); return; }
    fmt(buf, cap, "LPSS PWM #1: %s, %d%% (control %08x)", bl.on ? "on" : "off (sleeping)", bl.level, *bl.ctrl);
}
