/* Settings: theme, orientation, sleep, brightness, volume and power - persisted in UEFI NVRAM. */
#include "../ui/shell.h"
#include "../drivers/backlight.h"

static struct { tap_t tap; scroll_t sc; int confirm; int fw_mode; } st;   /* confirm: pending power action; fw_mode: cached QrtBootMode == 1 */

static const int sleep_secs[] = { 0, 30, 60, 120, 300, 600 };
static const char *sleep_label[] = { "Never", "30 s", "1 min", "2 min", "5 min", "10 min" };
#define N_SLEEP 6

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    for (int i = 0; i < 8; i++) {
        float a = i * PI_F / 4;
        gfx_line(c, cx + fsin(a) * r * 0.55f, cy - fcos(a) * r * 0.55f,
                 cx + fsin(a) * r * 0.95f, cy - fcos(a) * r * 0.95f, r * 0.3f, fg);
    }
    gfx_ring(c, cx, cy, r * 0.62f, r * 0.3f, fg);
}

/* a level row: [-]  track  [+] */
typedef struct { rect_t minus, track, plus; } level_t;

typedef struct {
    rect_t accent[N_ACCENTS], rot[4], sleep[N_SLEEP], kern[2], power[3], card;
    level_t bright, vol;
    int y_accent, y_rot, y_sleep, y_bright, y_vol, y_kern, y_power, y_about, bottom;
} lay_t;

static level_t level_row(int x, int y, int w) {
    int b = dp(44);
    return (level_t){ { x, y, b, b }, { x + b + dp(16), y + b / 2 - dp(4), w - 2 * b - dp(32), dp(8) }, { x + w - b, y, b, b } };
}

static lay_t layout(rect_t a) {
    lay_t L;
    int x = a.x + dp(40), w = a.w - dp(80);
    L.card = (rect_t){ a.x + dp(16), a.y, a.w - dp(32), a.h - dp(8) };
    int y = a.y + dp(24) - st.sc.off;
    int cw = (w - (N_ACCENTS / 2 - 1) * dp(10)) / (N_ACCENTS / 2);
    if (ui.landscape) cw = (w - (N_ACCENTS - 1) * dp(10)) / N_ACCENTS;
    L.y_accent = y;
    y += ui.small->line + dp(8);
    for (int i = 0; i < N_ACCENTS; i++) {
        int per = ui.landscape ? N_ACCENTS : N_ACCENTS / 2;
        L.accent[i] = (rect_t){ x + (i % per) * (cw + dp(10)), y + (i / per) * dp(54), cw, dp(44) };
    }
    y += (ui.landscape ? 1 : 2) * dp(54) + dp(16);
    L.y_rot = y;
    y += ui.small->line + dp(8);
    int rw = (w - 3 * dp(10)) / 4;
    for (int i = 0; i < 4; i++) L.rot[i] = (rect_t){ x + i * (rw + dp(10)), y, rw, dp(44) };
    y += dp(60);
    L.y_sleep = y;
    y += ui.small->line + dp(8);
    int per = ui.landscape ? N_SLEEP : 3;
    int sw = (w - (per - 1) * dp(10)) / per;
    for (int i = 0; i < N_SLEEP; i++) L.sleep[i] = (rect_t){ x + (i % per) * (sw + dp(10)), y + (i / per) * dp(54), sw, dp(44) };
    y += (N_SLEEP / per) * dp(54) + dp(6);
    L.y_bright = y;
    y += ui.small->line + dp(8);
    L.bright = level_row(x, y, w);
    y += dp(60);
    L.y_vol = y;
    y += ui.small->line + dp(8);
    L.vol = level_row(x, y, w);
    y += dp(60);
    L.y_kern = y;
    y += ui.small->line + dp(8);
    int kw = (w - dp(10)) / 2;
    for (int i = 0; i < 2; i++) L.kern[i] = (rect_t){ x + i * (kw + dp(10)), y, kw, dp(44) };
    y += dp(60);
    L.y_power = y;
    y += ui.small->line + dp(8);
    int pw = (w - 2 * dp(10)) / 3;
    for (int i = 0; i < 3; i++) L.power[i] = (rect_t){ x + i * (pw + dp(10)), y, pw, dp(52) };
    L.y_about = y + dp(76);
    L.bottom = L.y_about + ui.small->line + dp(6) + ui.label->line + 4 * ui.small->line + dp(24);
    return L;
}

static void draw_level(canvas_t *c, level_t lv, int pct, int enabled) {
    u32 fill = enabled ? RGBA(255, 255, 255, 30) : RGBA(255, 255, 255, 12);
    ui_button(c, lv.minus, "-", fill, enabled ? ui.text : ui.text3);
    ui_button(c, lv.plus, "+", fill, enabled ? ui.text : ui.text3);
    gfx_rrect(c, lv.track, lv.track.h / 2, RGBA(255, 255, 255, 36));
    if (enabled && pct > 0)
        gfx_rrect(c, (rect_t){ lv.track.x, lv.track.y, MAX(lv.track.h, lv.track.w * pct / 100), lv.track.h }, lv.track.h / 2, ui.accent);
    if (enabled) gfx_circle(c, lv.track.x + lv.track.w * pct / 100.0f, lv.track.y + lv.track.h / 2.0f, dp(10), ui.text);
}

/* -1, +1 for the buttons, 0..100 for a tap on the track, -1000 for a miss */
static int level_hit(level_t lv, int x, int y, int cur) {
    if (in_rect(lv.minus, x, y)) return cur - 10;
    if (in_rect(lv.plus, x, y)) return cur + 10;
    rect_t t = { lv.track.x - dp(10), lv.track.y - dp(18), lv.track.w + dp(20), lv.track.h + dp(36) };
    if (in_rect(t, x, y)) return CLAMP((x - lv.track.x) * 100 / MAX(1, lv.track.w), 0, 100);
    return -1000;
}

static const char *power_label[] = { "Restart", "Shut down", "Firmware" };

static void draw(canvas_t *c, rect_t a) {
    lay_t L = layout(a);
    int x = a.x + dp(40);
    ui_card(c, L.card, dp(22), 0);
    st.sc.max = MAX(0, L.bottom + st.sc.off - (a.y + a.h));
    gfx_clip(c, (rect_t){ L.card.x, L.card.y + dp(2), L.card.w, L.card.h - dp(4) });
    ui_section(c, x, L.y_accent, "ACCENT");
    for (int i = 0; i < N_ACCENTS; i++) {
        rect_t r = L.accent[i];
        int sel = i == shell_accent_index();
        gfx_rrect(c, r, r.h / 2, sel ? ALPHA(accent_palette[i], 255) : RGBA(255, 255, 255, 20));
        if (!sel) gfx_circle(c, r.x + dp(22), r.y + r.h / 2.0f, dp(8), accent_palette[i]);
        gfx_text_center(c, ui.label, (rect_t){ r.x + (sel ? 0 : dp(14)), r.y, r.w - (sel ? 0 : dp(14)), r.h }, accent_names[i], ui.text);
    }
    ui_section(c, x, L.y_rot, "SCREEN ROTATION");
    static const char *rot[] = { "0\xc2\xb0", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0" };
    for (int i = 0; i < 4; i++) ui_chip(c, L.rot[i], rot[i], i == shell_rotation());
    ui_section(c, x, L.y_sleep, "SLEEP AFTER (NO TOUCH OR BUTTON PRESS)");
    for (int i = 0; i < N_SLEEP; i++) ui_chip(c, L.sleep[i], sleep_label[i], sleep_secs[i] == shell_sleep_after());
    int bl = backlight_available();
    ui_section(c, x, L.y_bright, bl ? "BRIGHTNESS" : "BRIGHTNESS (NO BACKLIGHT CONTROL ON THIS MACHINE)");
    draw_level(c, L.bright, bl ? backlight_level() : 0, bl);
    ui_section(c, x, L.y_vol, "VOLUME (MOCK: NO SOUND DRIVER YET)");
    draw_level(c, L.vol, shell_volume(), 1);
    ui_section(c, x, L.y_kern, sizeof(void *) == 8 ? "KERNEL MODE (FROM NEXT BOOT)" : "KERNEL MODE");
    if (sizeof(void *) == 8) {
        int fw = st.fw_mode;                    /* draw() may run on any core: no firmware calls here */
        ui_chip(c, L.kern[0], "Native: QRT drives the hardware", !fw);
        ui_chip(c, L.kern[1], "Firmware: UEFI drivers underneath", fw);
    } else {
        gfx_text(c, ui.small, x, L.kern[0].y + (L.kern[0].h - ui.small->line) / 2,
                 "32-bit firmware: QRT always runs on top of UEFI", ui.text2);
    }
    ui_section(c, x, L.y_power, "POWER");
    for (int i = 0; i < 3; i++) {
        int armed = st.confirm == i + 1;
        ui_button(c, L.power[i], armed ? "Tap again" : power_label[i],
                  armed ? RGB(0xe5, 0x48, 0x4d) : RGBA(255, 255, 255, 30), ui.text);
    }
    int y = L.y_about;
    ui_section(c, x, y, "ABOUT");
    y += ui.small->line + dp(6);
    gfx_text(c, ui.label, x, y, "QRT " QRT_VERSION, ui.text);
    y += ui.label->line;
    static const char *about_native[] = {
        "Running natively: QRT exited the firmware and owns the machine -",
        "its own memory manager, scheduler, interrupts, cores and drivers.",
        "The firmware is kept only for the clock, NVRAM settings and reset.",
        "Linux x86-64 programs run in the Terminal.",
    };
    static const char *about_fw[] = {
        "Running on the firmware: Tessera keeps UEFI boot services alive and",
        "the tablet's own firmware drivers run display, touch, buttons,",
        "storage, clock and power underneath the shell.",
        "Settings live in UEFI NVRAM and survive reboots.",
    };
    const char **about = k.native ? about_native : about_fw;
    for (int i = 0; i < 4; i++, y += ui.small->line)
        gfx_text_fit(c, ui.small, x, y, L.card.w - dp(48), about[i], ui.text2);
    gfx_unclip(c);
}

static int event(const event_t *e, rect_t a) {
    int scrolled = scroll_event(&st.sc, e, a, dp(48));
    if (!tap_track(&st.tap, e, dp(12))) return scrolled;
    lay_t L = layout(a);
    for (int i = 0; i < N_SLEEP; i++) if (in_rect(L.sleep[i], e->x, e->y)) { shell_set_sleep_after(sleep_secs[i]); return 1; }
    int v;
    if (backlight_available() && (v = level_hit(L.bright, e->x, e->y, backlight_level())) != -1000) { backlight_set_level(v); return 1; }
    if ((v = level_hit(L.vol, e->x, e->y, shell_volume())) != -1000) { shell_set_volume(v); return 1; }
    for (int i = 0; i < N_ACCENTS; i++) if (in_rect(L.accent[i], e->x, e->y)) { shell_set_accent(i); return 1; }
    for (int i = 0; i < 4; i++) if (in_rect(L.rot[i], e->x, e->y)) { shell_set_rotation(i); return 1; }
    if (sizeof(void *) == 8)
        for (int i = 0; i < 2; i++)
            if (in_rect(L.kern[i], e->x, e->y)) { hal_setting_set(u"QrtBootMode", (u32)i); st.fw_mode = i; return 1; }
    for (int i = 0; i < 3; i++) {
        if (!in_rect(L.power[i], e->x, e->y)) continue;
        if (st.confirm != i + 1) { st.confirm = i + 1; return 1; }
        st.confirm = 0;
        if (i == 0) hal_reboot();
        else if (i == 1) hal_shutdown();
        else if (!hal_reboot_to_firmware()) klog("settings: firmware does not support boot-to-setup");
        return 1;
    }
    if (st.confirm) { st.confirm = 0; return 1; }
    return 0;
}

static void on_open(void) { st.confirm = 0; st.sc.off = 0; st.fw_mode = hal_setting_get(u"QrtBootMode", 0) == 1; }

const app_t app_settings = { "Settings", "Theme, sleep, brightness, sound, power", RGB(0x4d, 0xa3, 0xff), icon, on_open, draw, event, NULL };
