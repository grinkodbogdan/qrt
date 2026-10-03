/* Settings: a preferences page in the style of GNOME Settings.  Everything
 * is kept in UEFI NVRAM and survives reboots. */
#include "../ui/shell.h"
#include "../kernel/sound.h"
#include "../drivers/backlight.h"
#include "../drivers/i915/gpu.h"
#include "../drivers/i915/display.h"

static const int sleep_secs[] = { 0, 30, 60, 120, 300, 600 };
static const char *sleep_label[] = { "Never", "30 s", "1 min", "2 min", "5 min", "10 min" };
#define N_SLEEP 6
static const char *rot_label[] = { "0\xc2\xb0", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0" };
static const char *power_label[] = { "Restart", "Shut down", "Firmware setup" };

enum { SL_NONE, SL_BRIGHT, SL_VOL };

static struct {
    tap_t tap;
    scroll_t sc;
    int confirm;          /* power button waiting for a second tap (1..3) */
    int fw_mode;          /* cached QrtBootMode == 1 */
    int gpu_on;           /* cached gpu_enabled() */
    int ext_on;           /* cached display_enabled() */
    int slider;           /* the slider being dragged */
    int cand, x0, y0;     /* a touch that began on a slider: sideways moves it, up/down scrolls */
} st;

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    for (int i = 0; i < 8; i++) {
        float a = i * PI_F / 4;
        gfx_line(c, cx + fsin(a) * r * 0.55f, cy - fcos(a) * r * 0.55f,
                 cx + fsin(a) * r * 0.95f, cy - fcos(a) * r * 0.95f, r * 0.3f, fg);
    }
    gfx_ring(c, cx, cy, r * 0.62f, r * 0.3f, fg);
}

/* ---- layout ---------------------------------------------------------------------- */
#define ROW_H dp(58)

typedef struct {
    int x, w;                                   /* the clamped column */
    int y_look, y_power, y_sound, y_time, y_start, y_system, y_about;
    rect_t accent[N_ACCENTS], rot[4], sleep[N_SLEEP], kern[2], gpu[2], ext[2], desk[2], power[3], tz[2], test;
    rect_t bright, vol;                         /* slider tracks */
    rect_t g_look, g_power, g_sound, g_time, g_start, g_system, g_about;   /* group boxes */
    int bottom;
} lay_t;

static int bl(void) { return backlight_available(); }

static lay_t layout(rect_t a) {
    lay_t L;
    L.w = MIN(a.w - dp(48), dp(680));
    L.x = a.x + (a.w - L.w) / 2;
    int y = a.y + dp(24) - st.sc.off, x = L.x, w = L.w, pad = dp(16);
    int title = ui.label->line + dp(10);

    /* Appearance: accent, rotation */
    L.y_look = y; y += title;
    L.g_look = (rect_t){ x, y, w, 2 * ROW_H };
    int dot = dp(30), dx = x + w - pad - N_ACCENTS * (dot + dp(10)) + dp(10);
    for (int i = 0; i < N_ACCENTS; i++) L.accent[i] = (rect_t){ dx + i * (dot + dp(10)), y + (ROW_H - dot) / 2, dot, dot };
    int segw = MIN(dp(76), (w / 2) / 4);
    for (int i = 0; i < 4; i++) L.rot[i] = (rect_t){ x + w - pad - 4 * segw + i * segw, y + ROW_H + dp(11), segw, ROW_H - dp(22) };
    y += L.g_look.h + dp(24);

    /* Power: sleep, brightness */
    L.y_power = y; y += title;
    int prow = bl() ? 3 : 2;
    L.g_power = (rect_t){ x, y, w, prow * ROW_H };
    int sw = (w - 2 * pad) / N_SLEEP;
    for (int i = 0; i < N_SLEEP; i++) L.sleep[i] = (rect_t){ x + pad + i * sw, y + ROW_H + dp(8), sw, ROW_H - dp(20) };
    L.bright = (rect_t){ x + w / 2, y + 2 * ROW_H + ROW_H / 2 - dp(3), w / 2 - pad, dp(6) };
    y += L.g_power.h + dp(24);

    /* Sound */
    L.y_sound = y; y += title;
    L.g_sound = (rect_t){ x, y, w, 2 * ROW_H };
    L.vol = (rect_t){ x + w / 2, y + ROW_H / 2 - dp(3), w / 2 - pad, dp(6) };
    int tbw = MIN(dp(110), w / 4);
    L.test = (rect_t){ x + w - pad - tbw, y + ROW_H + dp(10), tbw, ROW_H - dp(20) };
    y += L.g_sound.h + dp(24);

    /* Date & time: the clock, the time zone */
    L.y_time = y; y += title;
    L.g_time = (rect_t){ x, y, w, 2 * ROW_H };
    int tw = MIN(dp(64), w / 6);
    for (int i = 0; i < 2; i++) L.tz[i] = (rect_t){ x + w - pad - 2 * tw + i * tw, y + ROW_H + dp(11), tw, ROW_H - dp(22) };
    y += L.g_time.h + dp(24);

    /* Startup (64-bit only) */
    L.y_start = y;
    if (sizeof(void *) == 8) {
        y += title;
        L.g_start = (rect_t){ x, y, w, 4 * ROW_H };
        int kw = MIN(dp(130), w / 4);
        for (int i = 0; i < 2; i++) L.kern[i] = (rect_t){ x + w - pad - 2 * kw + i * kw, y + dp(11), kw, ROW_H - dp(22) };
        for (int i = 0; i < 2; i++) L.gpu[i] = (rect_t){ x + w - pad - 2 * kw + i * kw, y + ROW_H + dp(11), kw, ROW_H - dp(22) };
        for (int i = 0; i < 2; i++) L.ext[i] = (rect_t){ x + w - pad - 2 * kw + i * kw, y + 2 * ROW_H + dp(11), kw, ROW_H - dp(22) };
        for (int i = 0; i < 2; i++) L.desk[i] = (rect_t){ x + w - pad - 2 * kw + i * kw, y + 3 * ROW_H + dp(11), kw, ROW_H - dp(22) };
        y += L.g_start.h + dp(24);
    } else L.g_start = (rect_t){ 0 };

    /* System: restart, shut down, firmware */
    L.y_system = y; y += title;
    L.g_system = (rect_t){ x, y, w, ROW_H };
    int bw = (w - 2 * pad - 2 * dp(10)) / 3;
    for (int i = 0; i < 3; i++) L.power[i] = (rect_t){ x + pad + i * (bw + dp(10)), y + dp(10), bw, ROW_H - dp(20) };
    y += L.g_system.h + dp(24);

    /* About */
    L.y_about = y; y += title;
    L.g_about = (rect_t){ x, y, w, 4 * ROW_H };
    y += L.g_about.h + dp(24);
    L.bottom = y + st.sc.off;
    return L;
}

/* ---- drawing ----------------------------------------------------------------------- */
static void group(canvas_t *c, rect_t g, int ty, const char *title, int rows) {
    gfx_text(c, ui.label, g.x + dp(4), ty, title, ui.text);
    gfx_rrect(c, g, dp(12), RGBA(255, 255, 255, 12));
    for (int i = 1; i < rows; i++) gfx_fill(c, (rect_t){ g.x, g.y + i * ROW_H, g.w, 1 }, RGBA(0, 0, 0, 80));
}

static void row_label(canvas_t *c, rect_t g, int row, const char *label, const char *sub) {
    int y = g.y + row * ROW_H;
    if (sub) {
        gfx_text(c, ui.body, g.x + dp(16), y + ROW_H / 2 - ui.body->line + dp(2), label, ui.text);
        gfx_text(c, ui.small, g.x + dp(16), y + ROW_H / 2 + dp(2), sub, ui.text2);
    } else gfx_text(c, ui.body, g.x + dp(16), y + (ROW_H - ui.body->line) / 2, label, ui.text);
}

static void segment(canvas_t *c, rect_t r, const char *label, int sel, int first, int last) {
    (void)first; (void)last;
    gfx_rrect(c, (rect_t){ r.x + 1, r.y, r.w - 2, r.h }, dp(8), sel ? ui.accent : RGBA(255, 255, 255, 18));
    gfx_text_center(c, sel ? ui.label : ui.body, r, label, ui.text);
}

static void slider(canvas_t *c, rect_t t, int pct) {
    gfx_rrect(c, t, t.h / 2, RGBA(255, 255, 255, 40));
    int fill = t.w * pct / 100;
    if (fill > 0) gfx_rrect(c, (rect_t){ t.x, t.y, MAX(t.h, fill), t.h }, t.h / 2, ui.accent);
    float kx = t.x + fill, ky = t.y + t.h / 2.0f;
    gfx_circle(c, kx, ky + dp(1), dp(11), RGBA(0, 0, 0, 80));
    gfx_circle(c, kx, ky, dp(10), RGB(0xff, 0xff, 0xff));
}

static void draw(canvas_t *c, rect_t a) {
    lay_t L = layout(a);
    st.sc.max = MAX(0, L.bottom - (a.y + a.h));
    rect_t old = c->clip;
    gfx_clip(c, a);

    group(c, L.g_look, L.y_look, "Appearance", 2);
    row_label(c, L.g_look, 0, "Accent colour", NULL);
    for (int i = 0; i < N_ACCENTS; i++) {
        rect_t r = L.accent[i];
        float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
        if (i == shell_accent_index()) gfx_ring(c, cx, cy, r.w / 2.0f + dp(3), dp(2), ui.text);
        gfx_circle(c, cx, cy, r.w / 2.0f - dp(2), accent_palette[i]);
    }
    row_label(c, L.g_look, 1, "Screen rotation", NULL);
    for (int i = 0; i < 4; i++) segment(c, L.rot[i], rot_label[i], i == shell_rotation(), i == 0, i == 3);

    group(c, L.g_power, L.y_power, "Power", bl() ? 3 : 2);
    row_label(c, L.g_power, 0, "Automatic sleep", "Lock and turn the screen off after");
    for (int i = 0; i < N_SLEEP; i++) segment(c, L.sleep[i], sleep_label[i], sleep_secs[i] == shell_sleep_after(), i == 0, i == N_SLEEP - 1);
    if (bl()) { row_label(c, L.g_power, 2, "Screen brightness", NULL); slider(c, L.bright, backlight_level()); }

    group(c, L.g_sound, L.y_sound, "Sound", 2);
    row_label(c, L.g_sound, 0, "Volume", NULL);
    slider(c, L.vol, shell_volume());
    const char *out = snd_output_name();
    row_label(c, L.g_sound, 1, "Output", out[0] ? out : "None: plug in USB audio, or pair Bluetooth headphones");
    ui_button(c, L.test, "Test", RGBA(255, 255, 255, 22), ui.text);

    {
        static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        EFI_TIME t;
        k_walltime(&t);
        char now[40], zone[48];
        fmt(now, sizeof now, "%u %s %u  %02u:%02u", t.Day, mon[(t.Month + 11) % 12], t.Year, t.Hour, t.Minute);
        int z = time_zone();
        fmt(zone, sizeof zone, "UTC%c%d:%02d%s", z < 0 ? '-' : '+', ABS_I(z) / 3600, ABS_I(z) / 60 % 60, time_zone_known() ? "" : " (not set yet)");
        group(c, L.g_time, L.y_time, "Date & time", 2);
        row_label(c, L.g_time, 0, "Date and time", time_synced() ? "Set from the network" : "Waiting for the network (secure sites need the right date)");
        int vw = text_width(ui.body, now);
        gfx_text(c, ui.body, L.g_time.x + L.g_time.w - dp(16) - vw, L.g_time.y + (ROW_H - ui.body->line) / 2, now, ui.text2);
        row_label(c, L.g_time, 1, "Time zone", zone);
        segment(c, L.tz[0], "-", 0, 1, 0);
        segment(c, L.tz[1], "+", 0, 0, 1);
    }

    if (sizeof(void *) == 8) {
        int fw = st.fw_mode;                     /* draw() may run on any core: no firmware calls here */
        group(c, L.g_start, L.y_start, "Startup", 4);
        row_label(c, L.g_start, 0, "Kernel mode", k.boot_note[0] ? k.boot_note : "Applies after a restart");
        segment(c, L.kern[0], "Native", !fw, 1, 0);
        segment(c, L.kern[1], "Firmware", fw, 0, 1);
        row_label(c, L.g_start, 1, "Graphics acceleration", gpu_status());
        segment(c, L.gpu[0], "On", st.gpu_on, 1, 0);
        segment(c, L.gpu[1], "Off", !st.gpu_on, 0, 1);
        row_label(c, L.g_start, 2, "External display (USB-C)", display_status());
        segment(c, L.ext[0], "On", st.ext_on, 1, 0);
        segment(c, L.ext[1], "Off", !st.ext_on, 0, 1);
        int da = shell_desk_auto();
        row_label(c, L.g_start, 3, "When a monitor is connected", da ? "The tablet becomes its touchpad and keyboard" : "The tablet's screen is mirrored to it");
        segment(c, L.desk[0], "Control", da, 1, 0);
        segment(c, L.desk[1], "Mirror", !da, 0, 1);
    }

    group(c, L.g_system, L.y_system, "System", 1);
    for (int i = 0; i < 3; i++) {
        int armed = st.confirm == i + 1;
        ui_button(c, L.power[i], armed ? "Tap again" : power_label[i],
                  armed || i == 1 ? RGB(0xc0, 0x1c, 0x28) : RGBA(255, 255, 255, 22), ui.text);
    }

    group(c, L.g_about, L.y_about, "About", 4);
    char ram[24];
    fmt_bytes(ram, sizeof ram, k.ram_bytes);
    const char *keys[4] = { "Operating system", "Device", "Processor", "Memory" };
    const char *vals[4] = { "QRT " QRT_VERSION, k.sys_product[0] ? k.sys_product : "PC", k.cpu, ram };
    for (int i = 0; i < 4; i++) {
        int y = L.g_about.y + i * ROW_H + (ROW_H - ui.body->line) / 2;
        gfx_text(c, ui.body, L.g_about.x + dp(16), y, keys[i], ui.text);
        int vw = MIN(text_width(ui.body, vals[i]), L.g_about.w / 2);
        gfx_text_fit(c, ui.body, L.g_about.x + L.g_about.w - dp(16) - vw, y, L.g_about.w / 2, vals[i], ui.text2);
    }
    c->clip = old;
}

/* ---- input ------------------------------------------------------------------------ */
static int slider_value(rect_t t, int x) { return CLAMP((x - t.x) * 100 / MAX(1, t.w), 0, 100); }
static rect_t grab(rect_t t) { return (rect_t){ t.x - dp(14), t.y - dp(20), t.w + dp(28), t.h + dp(40) }; }

static void set_slider(int which, int v) {
    if (which == SL_BRIGHT) backlight_set_level(v);
    else if (which == SL_VOL) shell_set_volume(v);
}

static int event(const event_t *e, rect_t a) {
    lay_t L = layout(a);
    /* a slider follows a sideways drag or a tap; a vertical swipe scrolls the page */
    if (e->type == EV_DOWN) {
        st.slider = SL_NONE;
        st.cand = bl() && in_rect(grab(L.bright), e->x, e->y) ? SL_BRIGHT : in_rect(grab(L.vol), e->x, e->y) ? SL_VOL : SL_NONE;
        st.x0 = e->x; st.y0 = e->y;
    } else if (e->type == EV_MOVE && st.cand && !st.slider) {
        int dx = ABS_I(e->x - st.x0), dy = ABS_I(e->y - st.y0);
        if (dx > dp(6) && dx >= dy) st.slider = st.cand;
        else if (dy > dp(6)) st.cand = SL_NONE;
    }
    if (st.slider) {
        rect_t t = st.slider == SL_BRIGHT ? L.bright : L.vol;
        set_slider(st.slider, slider_value(t, e->x));
        if (e->type == EV_UP) { st.slider = st.cand = SL_NONE; tap_track(&st.tap, e, dp(12)); }
        return 1;
    }
    int scrolled = scroll_event(&st.sc, e, a, dp(48));
    if (!tap_track(&st.tap, e, dp(12))) return scrolled;
    if (st.cand) { set_slider(st.cand, slider_value(st.cand == SL_BRIGHT ? L.bright : L.vol, e->x)); st.cand = SL_NONE; return 1; }
    for (int i = 0; i < N_ACCENTS; i++) if (in_rect(grab(L.accent[i]), e->x, e->y)) { shell_set_accent(i); return 1; }
    for (int i = 0; i < 4; i++) if (in_rect(L.rot[i], e->x, e->y)) { shell_set_rotation(i); return 1; }
    for (int i = 0; i < N_SLEEP; i++) if (in_rect(L.sleep[i], e->x, e->y)) { shell_set_sleep_after(sleep_secs[i]); return 1; }
    if (in_rect(L.test, e->x, e->y)) { snd_beep(); return 1; }
    for (int i = 0; i < 2; i++)
        if (in_rect(L.tz[i], e->x, e->y)) { time_set_zone(time_zone() + (i ? 1800 : -1800)); shell_redraw(); return 1; }
    if (sizeof(void *) == 8)
        for (int i = 0; i < 2; i++)
            if (in_rect(L.kern[i], e->x, e->y)) { hal_setting_set(u"QrtBootMode", (u32)i); st.fw_mode = i; return 1; }
    if (sizeof(void *) == 8 && gpu_supported())
        for (int i = 0; i < 2; i++)
            if (in_rect(L.gpu[i], e->x, e->y)) {
                gpu_set_enabled(i == 0);
                st.gpu_on = gpu_enabled();
                shell_redraw();
                return 1;
            }
    if (sizeof(void *) == 8)
        for (int i = 0; i < 2; i++) {
            if (in_rect(L.desk[i], e->x, e->y)) { shell_set_desk_auto(i == 0); shell_redraw(); return 1; }
            if (in_rect(L.ext[i], e->x, e->y)) {
                display_set_enabled(i == 0);
                st.ext_on = display_enabled();
                shell_redraw();
                return 1;
            }
        }
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
    return scrolled;
}

static void on_open(void) { st.confirm = 0; st.sc.off = 0; st.fw_mode = hal_setting_get(u"QrtBootMode", 0) == 1; st.gpu_on = gpu_enabled(); st.ext_on = display_enabled(); }

const app_t app_settings = { "Settings", "Appearance, power, sound", RGB(0x6f, 0x6f, 0x78), icon, on_open, draw, event, NULL };
