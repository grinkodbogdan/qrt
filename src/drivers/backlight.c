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
 *
 * 0.9.5: two more ways, tried in order when that PWM is not running:
 *  - the Crystal Cove PMIC's PWM0 (Linux's pwm-crc.c): BACKLIGHT_EN 0x51,
 *    PWM0_CLK_DIV 0x4B (bit 7 output on), PWM0_DUTY_CYCLE 0x4E (0..255),
 *    used by i915 for DSI panels whose VBT says "PMIC"; taken only if the
 *    firmware left it on;
 *  - software: the shell darkens what it draws (backlight_dim_alpha), so the
 *    slider always does something - it saves no power.
 */
#include "backlight.h"
#include "touch.h"
#include "pmic.h"

#define GNVS_P10A 376          /* PWM #1 MMIO base (DSDT field offset) */
#define PWM_ENABLE (1u << 31)
#define PWM_UPDATE (1u << 30)

enum { BL_NONE, BL_LPSS, BL_PMIC, BL_SOFT };
static struct { volatile u32 *ctrl; u32 saved; int level, on, mode; } bl;
static u64 save_at;

#define CRC_PWM0_CLK_DIV 0x4b
#define CRC_PWM0_DUTY    0x4e
#define CRC_BL_EN        0x51

static void write_level(int pct) {
    if (bl.mode == BL_PMIC) { pmic_write(CRC_PWM0_DUTY, (u8)((255 * pct + 50) / 100)); return; }
    if (bl.mode != BL_LPSS) return;
    u32 div = (u32)(255 - (255 * pct + 50) / 100);
    u32 v = (bl.saved & ~0xffu & ~PWM_UPDATE) | PWM_ENABLE | div;
    *bl.ctrl = v;
    *bl.ctrl = v | PWM_UPDATE;
}

static int start(int mode, int cur) {
    bl.mode = mode;
    bl.level = (int)hal_setting_get(u"QrtBrightness", (u32)CLAMP(cur, 5, 100));
    bl.level = CLAMP(bl.level, 5, 100);
    bl.on = 1;
    write_level(bl.level);
    return mode != BL_SOFT;
}

static int try_lpss(void) {
    u32 g = venue_gnvs();
    if (!g) return 0;
    u32 base = *(volatile u32 *)(usize)(g + GNVS_P10A);
    if (!base || base == 0xffffffffu) { klog("backlight: no PWM base in NVS"); return 0; }
    volatile u32 *ctrl = (volatile u32 *)(usize)base;
    u32 v = *ctrl;
    klog("backlight: PWM #1 at %08x, control %08x", base, v);
    if (v == 0xffffffffu || !(v & PWM_ENABLE)) { klog("backlight: PWM #1 not running; left alone"); return 0; }
    bl.ctrl = ctrl;
    bl.saved = v & ~PWM_UPDATE;
    return start(BL_LPSS, (int)((255 - (v & 0xff)) * 100 / 255));
}

static int try_pmic(void) {
    if (pmic_kind() != PMIC_CRYSTAL_COVE) return 0;
    u8 en = 0, div = 0, duty = 0;
    if (pmic_read(CRC_BL_EN, &en) || pmic_read(CRC_PWM0_CLK_DIV, &div) || pmic_read(CRC_PWM0_DUTY, &duty)) return 0;
    klog("backlight: Crystal Cove PWM0: enable %02x, divider %02x, duty %02x", en, div, duty);
    if (!(en & 1) || !(div & 0x80)) { klog("backlight: PMIC PWM not running; left alone"); return 0; }
    return start(BL_PMIC, duty * 100 / 255);
}

int backlight_init(void) {
    if (bl.mode) return bl.mode != BL_SOFT;
    if (k.native && k.is_venue && (try_lpss() || try_pmic())) return 1;
    start(BL_SOFT, 100);                     /* the slider still works: the shell dims the picture */
    return 0;
}

int backlight_available(void) { return bl.mode == BL_LPSS || bl.mode == BL_PMIC; }
int backlight_level(void) { if (!bl.mode) backlight_init(); return bl.level; }

/* software dimming: how dark a veil the shell lays over the picture (0 = none) */
int backlight_dim_alpha(void) { return bl.mode == BL_SOFT && bl.level < 100 ? (100 - bl.level) * 200 / 95 : 0; }

void backlight_set_level(int pct) {
    if (!bl.mode) backlight_init();
    bl.level = CLAMP(pct, 5, 100);
    if (bl.on) write_level(bl.level);
    save_at = k_now_ms() + 1500;            /* NVRAM is flash: once the slider rests, not on every step */
}

void backlight_tick(void) {
    if (save_at && k_now_ms() >= save_at) { save_at = 0; hal_setting_set(u"QrtBrightness", (u32)bl.level); }
}

void backlight_power(int on) {
    if (!backlight_available() || bl.on == on) return;
    bl.on = on;
    if (on) write_level(bl.level);
    else if (bl.mode == BL_PMIC) pmic_write(CRC_PWM0_DUTY, 0);
    else { write_level(0); *bl.ctrl = (*bl.ctrl & ~PWM_ENABLE) | PWM_UPDATE; }
}

void backlight_status(char *buf, usize cap) {
    switch (bl.mode) {
    case BL_LPSS: fmt(buf, cap, "LPSS PWM #1: %s, %d%% (control %08x)", bl.on ? "on" : "off (sleeping)", bl.level, *bl.ctrl); break;
    case BL_PMIC: fmt(buf, cap, "Crystal Cove PMIC PWM0: %s, %d%%", bl.on ? "on" : "off (sleeping)", bl.level); break;
    default: fmt(buf, cap, "no backlight control found (%s); dimmed in software, %d%%", k.native ? "PWM #1 and the PMIC's PWM are off" : "firmware mode", bl.level); break;
    }
}
