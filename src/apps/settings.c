/* Settings: theme, orientation and power - persisted in UEFI NVRAM. */
#include "../ui/shell.h"

static struct { tap_t tap; int confirm; } st;   /* confirm: pending power action */

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    for (int i = 0; i < 8; i++) {
        float a = i * PI_F / 4;
        gfx_line(c, cx + fsin(a) * r * 0.55f, cy - fcos(a) * r * 0.55f,
                 cx + fsin(a) * r * 0.95f, cy - fcos(a) * r * 0.95f, r * 0.3f, fg);
    }
    gfx_ring(c, cx, cy, r * 0.62f, r * 0.3f, fg);
}

typedef struct { rect_t accent[N_ACCENTS], rot[4], power[3], card; int y_accent, y_rot, y_power, y_about; } lay_t;

static lay_t layout(rect_t a) {
    lay_t L;
    int x = a.x + dp(40), w = a.w - dp(80);
    L.card = (rect_t){ a.x + dp(16), a.y, a.w - dp(32), a.h - dp(8) };
    int y = a.y + dp(24);
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
    L.y_power = y;
    y += ui.small->line + dp(8);
    int pw = (w - 2 * dp(10)) / 3;
    for (int i = 0; i < 3; i++) L.power[i] = (rect_t){ x + i * (pw + dp(10)), y, pw, dp(52) };
    L.y_about = y + dp(76);
    return L;
}

static const char *power_label[] = { "Restart", "Shut down", "Firmware" };

static void draw(canvas_t *c, rect_t a) {
    lay_t L = layout(a);
    int x = a.x + dp(40);
    ui_card(c, L.card, dp(22), 0);
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
    static const char *about[] = {
        "Tessera is a firmware-hosted exokernel: it never exits UEFI boot",
        "services, so the tablet's own firmware drivers run display, touch,",
        "buttons, eMMC/SD/USB storage, clock and power underneath the shell.",
        "Settings live in UEFI NVRAM and survive reboots.",
    };
    for (int i = 0; i < 4; i++, y += ui.small->line)
        gfx_text_fit(c, ui.small, x, y, L.card.w - dp(48), about[i], ui.text2);
}

static int event(const event_t *e, rect_t a) {
    if (!tap_track(&st.tap, e, dp(12))) return 0;
    lay_t L = layout(a);
    for (int i = 0; i < N_ACCENTS; i++) if (in_rect(L.accent[i], e->x, e->y)) { shell_set_accent(i); return 1; }
    for (int i = 0; i < 4; i++) if (in_rect(L.rot[i], e->x, e->y)) { shell_set_rotation(i); return 1; }
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

static void on_open(void) { st.confirm = 0; }

const app_t app_settings = { "Settings", "Theme, rotation, power", RGB(0x4d, 0xa3, 0xff), icon, on_open, draw, event, NULL };
