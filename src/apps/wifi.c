/* Wi-Fi: in the style of GNOME's Wi-Fi panel - a switch, the connected
 * network, the visible networks, and a password dialog.  The driver log is
 * one tap away under "Diagnostics". */
#include "../ui/shell.h"
#include "../net/wlan.h"
#include "../net/net.h"
#include "../net/wifilog.h"
#include "../drivers/wifi.h"
#include "../net/netstack.h"

static struct {
    scroll_t sc;
    tap_t tap;
    int pressed;                 /* row or button under the finger */
    int show_log;
    int pending_power;           /* 1 on, 2 off: done in tick after a frame shows "starting" */
    int sheet;                   /* password dialog open */
    char sheet_ssid[33];
    char pass[64];
    int show_pass;
    char note[96];
    wlan_net_t nets[32];
    int n_nets;
    u32 log_seen;
    u64 last_refresh;
} st = { .pressed = -1 };

enum { HIT_SWITCH = 150, HIT_SCAN, HIT_DISCONNECT, HIT_FORGET, HIT_DIAG, HIT_SAVE, HIT_BACK };

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    float by = cy + r * 0.55f;
    for (int i = 1; i <= 3; i++) {
        float rr = r * (0.3f + 0.36f * i);
        gfx_line(c, cx - rr * 0.72f, by - rr * 0.66f, cx, by - rr, r * 0.15f, fg);
        gfx_line(c, cx, by - rr, cx + rr * 0.72f, by - rr * 0.66f, r * 0.15f, fg);
    }
    gfx_circle(c, cx, by - r * 0.1f, r * 0.15f, fg);
}

/* ---- layout ------------------------------------------------------------------ */
#define ROW_H dp(56)
static int col_w(rect_t a) { return MIN(a.w - dp(48), dp(680)); }
static int col_x(rect_t a) { return a.x + (a.w - col_w(a)) / 2; }
static rect_t switch_group(rect_t a) { return (rect_t){ col_x(a), a.y + dp(24), col_w(a), ROW_H }; }
static rect_t switch_rect(rect_t a) { rect_t g = switch_group(a); return (rect_t){ g.x + g.w - dp(16) - dp(52), g.y + (ROW_H - dp(30)) / 2, dp(52), dp(30) }; }
static int connected(void) { return wlan_state() == WL_CONNECTED; }
static rect_t current_group(rect_t a) { rect_t s = switch_group(a); return (rect_t){ s.x, s.y + s.h + dp(24) + ui.label->line + dp(10), s.w, connected() ? ROW_H : 0 }; }
static rect_t disconnect_btn(rect_t a) { rect_t g = current_group(a); int w = dp(120); return (rect_t){ g.x + g.w - dp(12) - w, g.y + dp(11), w, ROW_H - dp(22) }; }
static rect_t forget_btn(rect_t a) { rect_t d = disconnect_btn(a); return (rect_t){ d.x - dp(10) - dp(100), d.y, dp(100), d.h }; }
static int list_top(rect_t a) {
    rect_t cg = current_group(a);
    int y = connected() ? cg.y + cg.h + dp(24) : switch_group(a).y + ROW_H + dp(24);
    return y + ui.label->line + dp(10);
}
static rect_t list_rect(rect_t a) { int y = list_top(a); return (rect_t){ col_x(a), y, col_w(a), a.y + a.h - y - dp(60) }; }
static rect_t scan_btn(rect_t a) { rect_t l = list_rect(a); return (rect_t){ l.x + l.w - dp(40), l.y - ui.label->line - dp(14), dp(40), dp(34) }; }
static rect_t diag_btn(rect_t a) { return (rect_t){ col_x(a), a.y + a.h - dp(48), dp(140), dp(36) }; }
static rect_t save_btn(rect_t a) { rect_t d = diag_btn(a); return (rect_t){ d.x + d.w + dp(10), d.y, dp(140), d.h }; }
static rect_t sheet_rect(rect_t a) { int w = MIN(a.w - dp(32), dp(480)); return (rect_t){ a.x + (a.w - w) / 2, a.y + dp(24), w, dp(230) }; }
static rect_t sheet_field(rect_t s) { return (rect_t){ s.x + dp(20), s.y + dp(66), s.w - dp(40), dp(44) }; }
static rect_t sheet_btn(rect_t s, int i) {       /* Show, Cancel, Connect */
    int w = (s.w - dp(40) - 2 * dp(10)) / 3;
    return (rect_t){ s.x + dp(20) + i * (w + dp(10)), s.y + s.h - dp(20) - dp(44), w, dp(44) };
}

/* signal as a Wi-Fi fan, like GNOME's network-wireless-signal icons */
static void signal_icon(canvas_t *c, float x, float cy, int rssi, u32 on, u32 off) {
    int level = rssi >= -55 ? 3 : rssi >= -67 ? 2 : rssi >= -78 ? 1 : 0;
    float s = dp(20), cx = x + s / 2, by = cy + s * 0.4f;
    gfx_circle(c, cx, by - dp(1), dp(2), on);
    for (int k2 = 1; k2 <= 3; k2++) {
        float rr = s * 0.3f * k2;
        u32 col = k2 <= level ? on : off;
        float px = cx + rr * fcos(-2.4f), py = by + rr * fsin(-2.4f);
        for (int i = 1; i <= 8; i++) {
            float a = -2.4f + 1.65f * i / 8;
            float qx = cx + rr * fcos(a), qy = by + rr * fsin(a);
            gfx_line(c, px, py, qx, qy, dp(2), col);
            px = qx; py = qy;
        }
    }
}

static void lock_icon(canvas_t *c, float x, float cy, u32 col) {
    float w = dp(12), h = dp(9);
    gfx_rrect(c, (rect_t){ (int)x, (int)(cy - h / 2 + dp(3)), (int)w, (int)h }, dp(2), col);
    gfx_ring(c, x + w / 2, cy - dp(2), dp(4), dp(1.8f), col);
}

static void sw(canvas_t *c, rect_t r, int on) {
    gfx_rrect(c, r, r.h / 2, on ? ui.accent : RGBA(255, 255, 255, 50));
    gfx_circle(c, on ? r.x + r.w - r.h / 2.0f : r.x + r.h / 2.0f, r.y + r.h / 2.0f, r.h / 2.0f - dp(3), RGB(255, 255, 255));
}

static void state_line(char *out, usize cap) {
    const char *s = st.pending_power == 1 ? "Turning on..." : wlan_state_text();
    strlcpy(out, s, cap);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

static void draw(canvas_t *c, rect_t a) {
    int avail = wlan_available(), on = wlan_state() != WL_OFF;
    rect_t old = c->clip;
    gfx_clip(c, a);
    if (!avail) {
        rect_t mid = { a.x, a.y + a.h / 2 - dp(60), a.w, dp(40) };
        icon(c, a.x + a.w / 2.0f, mid.y - dp(30), dp(30), ui.text3);
        gfx_text_center(c, ui.title, mid, "No Wi-Fi adapter found", ui.text);
        const char *ns = net_status();
        if (ns) gfx_text_center(c, ui.body, (rect_t){ a.x, mid.y + dp(40), a.w, ui.body->line }, ns, ui.text2);
        c->clip = old;
        return;
    }
    if (st.show_log) {
        rect_t l = { col_x(a), a.y + dp(16), col_w(a), a.h - dp(80) };
        gfx_rrect(c, l, dp(12), RGB(0x1d, 0x1d, 0x20));
        const font_t *m = font_pick(F_MONO, dp(12));
        rect_t in = { l.x + dp(12), l.y + dp(10), l.w - dp(24), l.h - dp(20) };
        gfx_clip(c, in);
        int n = wifilog_count(), y = in.y - st.sc.off;
        for (int i = 0; i < n; i++, y += m->line)
            if (y + m->line > in.y && y < in.y + in.h) gfx_text_fit(c, m, in.x, y, in.w, wifilog_line(i), RGB(0xde, 0xdd, 0xda));
        gfx_clip(c, a);
        ui_button(c, diag_btn(a), "Back", RGBA(255, 255, 255, 22), ui.text);
        ui_button(c, save_btn(a), "Save to stick", RGBA(255, 255, 255, 22), ui.text);
        if (st.note[0]) gfx_text_fit(c, ui.small, save_btn(a).x + save_btn(a).w + dp(12), save_btn(a).y + dp(8), a.w / 2, st.note, ui.text2);
        c->clip = old;
        return;
    }
    /* the switch */
    rect_t g = switch_group(a);
    gfx_rrect(c, g, dp(12), RGBA(255, 255, 255, 12));
    char line[96];
    state_line(line, sizeof line);
    gfx_text(c, ui.body, g.x + dp(16), g.y + ROW_H / 2 - ui.body->line + dp(2), "Wi-Fi", ui.text);
    gfx_text_fit(c, ui.small, g.x + dp(16), g.y + ROW_H / 2 + dp(2), g.w - dp(100), line, ui.text2);
    sw(c, switch_rect(a), on || st.pending_power == 1);

    char saved[33] = { 0 };
    wlan_saved(saved, sizeof saved);
    if (connected()) {
        rect_t cg = current_group(a);
        gfx_text(c, ui.label, cg.x + dp(4), cg.y - ui.label->line - dp(10), "Connected", ui.text);
        gfx_rrect(c, cg, dp(12), RGBA(255, 255, 255, 12));
        const char *ssid = "";
        for (int i = 0; i < st.n_nets; i++) if (!strcmp(st.nets[i].ssid, saved)) ssid = st.nets[i].ssid;
        signal_icon(c, cg.x + dp(16), cg.y + ROW_H / 2.0f, -50, ui.text, RGBA(255, 255, 255, 50));
        const char *ns = net_status();
        gfx_text_fit(c, ui.body, cg.x + dp(52), cg.y + ROW_H / 2 - ui.body->line + dp(2), forget_btn(a).x - cg.x - dp(60), ssid[0] ? ssid : saved, ui.text);
        gfx_text_fit(c, ui.small, cg.x + dp(52), cg.y + ROW_H / 2 + dp(2), forget_btn(a).x - cg.x - dp(60), ns ? ns : "", ui.text2);
        ui_button(c, forget_btn(a), "Forget", RGBA(255, 255, 255, st.pressed == HIT_FORGET ? 50 : 22), ui.text);
        ui_button(c, disconnect_btn(a), "Disconnect", RGBA(255, 255, 255, st.pressed == HIT_DISCONNECT ? 50 : 22), ui.text);
    }
    if (on) {
        rect_t l = list_rect(a);
        gfx_text(c, ui.label, l.x + dp(4), l.y - ui.label->line - dp(10), "Visible networks", ui.text);
        /* refresh (scan) button: a circular arrow */
        rect_t sb = scan_btn(a);
        float cx = sb.x + sb.w / 2.0f, cy = sb.y + sb.h / 2.0f, s = dp(7);
        u32 fg = wlan_state() == WL_SCANNING ? ui.accent : ui.text;
        float px = cx + s, py = cy;
        for (int i = 1; i <= 12; i++) {
            float ang = 1.9f * PI_F * i / 12;
            float qx = cx + fcos(ang) * s, qy = cy - fsin(ang) * s;
            gfx_line(c, px, py, qx, qy, dp(2), fg);
            px = qx; py = qy;
        }
        gfx_line(c, cx + s, cy, cx + s - dp(4), cy - dp(4), dp(2), fg);
        gfx_line(c, cx + s, cy, cx + s + dp(4), cy - dp(4), dp(2), fg);

        int shown = 0;
        for (int i = 0; i < st.n_nets; i++) if (!(connected() && !strcmp(st.nets[i].ssid, saved))) shown++;
        if (!shown) {
            gfx_text(c, ui.body, l.x + dp(4), l.y + dp(8), wlan_state() == WL_SCANNING ? "Searching..." : "No networks found", ui.text2);
        } else {
            rect_t box = { l.x, l.y - st.sc.off, l.w, shown * ROW_H };
            gfx_clip(c, (rect_t){ l.x, l.y, l.w, l.h });
            gfx_rrect(c, box, dp(12), RGBA(255, 255, 255, 12));
            int row = 0;
            for (int i = 0; i < st.n_nets; i++) {
                wlan_net_t *nw = &st.nets[i];
                if (connected() && !strcmp(nw->ssid, saved)) continue;
                rect_t r = { box.x, box.y + row * ROW_H, box.w, ROW_H };
                if (row) gfx_fill(c, (rect_t){ r.x, r.y, r.w, 1 }, RGBA(0, 0, 0, 80));
                if (st.pressed == i) gfx_fill(c, (rect_t){ r.x, r.y + 1, r.w, r.h - 1 }, RGBA(255, 255, 255, 18));
                int ok = wlan_supported(nw->security);
                signal_icon(c, r.x + dp(16), r.y + ROW_H / 2.0f, nw->rssi, ok ? ui.text : ui.text3, RGBA(255, 255, 255, 40));
                gfx_text_fit(c, ui.body, r.x + dp(52), r.y + (ROW_H - ui.body->line) / 2, r.w - dp(200), nw->ssid, ok ? ui.text : ui.text3);
                const char *tag = !ok ? "Not supported" : !strcmp(saved, nw->ssid) ? "Saved" : "";
                int tw = text_width(ui.small, tag);
                gfx_text(c, ui.small, r.x + r.w - dp(48) - tw, r.y + (ROW_H - ui.small->line) / 2, tag, ui.text2);
                if (nw->security != SEC_OPEN) lock_icon(c, r.x + r.w - dp(32), r.y + ROW_H / 2.0f, ok ? ui.text2 : ui.text3);
                row++;
            }
            gfx_clip(c, a);
        }
    }
    if (st.note[0]) gfx_text_fit(c, ui.small, diag_btn(a).x + diag_btn(a).w + dp(12), diag_btn(a).y + dp(8), a.w - dp(200), st.note, ui.text2);
    ui_button(c, diag_btn(a), "Diagnostics", RGBA(255, 255, 255, st.pressed == HIT_DIAG ? 50 : 16), ui.text2);

    if (st.sheet) {
        gfx_fill(c, a, RGBA(0, 0, 0, 140));
        rect_t s = sheet_rect(a);
        gfx_rrect(c, s, dp(14), RGB(0x36, 0x36, 0x3a));
        gfx_rrect_outline(c, s, dp(14), 1, ui.stroke);
        char t[64];
        fmt(t, sizeof t, "Connect to %s", st.sheet_ssid);
        gfx_text_center(c, ui.label, (rect_t){ s.x + dp(20), s.y + dp(18), s.w - dp(40), ui.label->line }, t, ui.text);
        rect_t f = sheet_field(s);
        gfx_rrect(c, f, dp(8), RGBA(255, 255, 255, 18));
        gfx_rrect_outline(c, f, dp(8), dp(2), ui.accent);
        char shown[64];
        usize n = strlen(st.pass);
        if (st.show_pass) strlcpy(shown, st.pass, sizeof shown);
        else { usize i; for (i = 0; i < n && i < 40; i++) shown[i] = '*'; shown[i] = 0; }
        int tx = f.x + dp(12), ty = f.y + (f.h - ui.body->line) / 2;
        if (!n) gfx_text(c, ui.body, tx, ty, "Password", ui.text3);
        int end = gfx_text(c, ui.body, tx, ty, shown, ui.text);
        gfx_fill(c, (rect_t){ (n ? end : tx) + 1, f.y + dp(10), dp(2), f.h - dp(20) }, ui.accent);
        if (n && n < 8) gfx_text_fit(c, ui.small, f.x, f.y + f.h + dp(6), f.w, "At least 8 characters", ui.text2);
        static const char *sb[] = { "Show", "Cancel", "Connect" };
        for (int i = 0; i < 3; i++)
            ui_button(c, sheet_btn(s, i), i == 0 && st.show_pass ? "Hide" : sb[i],
                      i == 2 ? (n >= 8 ? ui.accent : ALPHA(ui.accent, 90)) : RGBA(255, 255, 255, st.pressed == 200 + i ? 60 : 26), ui.text);
    }
    c->clip = old;
}

static void open_sheet(const char *ssid) {
    st.sheet = 1;
    strlcpy(st.sheet_ssid, ssid, sizeof st.sheet_ssid);
    st.pass[0] = 0;
    st.show_pass = 0;
    shell_keyboard(1);
}

static void connect_to(const wlan_net_t *nw) {
    st.note[0] = 0;
    if (!wlan_supported(nw->security)) {
        fmt(st.note, sizeof st.note, "%s uses %s, which is not supported yet", nw->ssid, wlan_security_name(nw->security));
        return;
    }
    char saved[33];
    if (nw->security == SEC_OPEN || (wlan_saved(saved, sizeof saved) && !strcmp(saved, nw->ssid))) { net_lock(); wlan_connect(nw->ssid, NULL); net_unlock(); }
    else open_sheet(nw->ssid);
}

/* the network under a y position in the list, -1 if none */
static int row_at(rect_t a, int y) {
    rect_t l = list_rect(a);
    if (y < l.y || y >= l.y + l.h) return -1;
    char saved[33] = { 0 };
    wlan_saved(saved, sizeof saved);
    int r = (y - l.y + st.sc.off) / ROW_H, row = 0;
    for (int i = 0; i < st.n_nets; i++) {
        if (connected() && !strcmp(st.nets[i].ssid, saved)) continue;
        if (row++ == r) return i;
    }
    return -1;
}

static int event(const event_t *e, rect_t a) {
    if (st.sheet) {
        rect_t s = sheet_rect(a);
        if (e->type == EV_KEY) {
            usize n = strlen(st.pass);
            if (e->scan == SCAN_ESC) { st.sheet = 0; shell_keyboard(0); }
            else if (e->ch == '\r') { if (n >= 8) { st.sheet = 0; shell_keyboard(0); net_lock(); wlan_connect(st.sheet_ssid, st.pass); net_unlock(); } }
            else if (e->ch == 8) { if (n) st.pass[n - 1] = 0; }
            else if (e->ch >= 32 && e->ch < 127 && n + 1 < sizeof st.pass) { st.pass[n] = (char)e->ch; st.pass[n + 1] = 0; }
            return 1;
        }
        int hit = -1;
        for (int i = 0; i < 3; i++) if (in_rect(sheet_btn(s, i), e->x, e->y)) hit = 200 + i;
        if (e->type == EV_DOWN) st.pressed = hit;
        if (tap_track(&st.tap, e, dp(12))) {
            if (hit == 200) st.show_pass = !st.show_pass;
            else if (hit == 201) { st.sheet = 0; shell_keyboard(0); }
            else if (hit == 202 && strlen(st.pass) >= 8) { st.sheet = 0; shell_keyboard(0); net_lock(); wlan_connect(st.sheet_ssid, st.pass); net_unlock(); }
            else if (in_rect(sheet_field(s), e->x, e->y)) shell_keyboard(1);
        }
        if (e->type == EV_UP) st.pressed = -1;
        return 1;
    }
    rect_t l = st.show_log ? (rect_t){ col_x(a), a.y + dp(16), col_w(a), a.h - dp(80) } : list_rect(a);
    if (e->type == EV_KEY) return scroll_event(&st.sc, e, l, dp(40));
    if (scroll_event(&st.sc, e, l, dp(40))) { st.pressed = -1; return 1; }
    int hit = -1;
    if (in_rect(diag_btn(a), e->x, e->y)) hit = st.show_log ? HIT_BACK : HIT_DIAG;
    else if (st.show_log && in_rect(save_btn(a), e->x, e->y)) hit = HIT_SAVE;
    else if (!st.show_log) {
        if (in_rect(switch_group(a), e->x, e->y)) hit = HIT_SWITCH;
        else if (connected() && in_rect(disconnect_btn(a), e->x, e->y)) hit = HIT_DISCONNECT;
        else if (connected() && in_rect(forget_btn(a), e->x, e->y)) hit = HIT_FORGET;
        else if (wlan_state() != WL_OFF && in_rect(scan_btn(a), e->x, e->y)) hit = HIT_SCAN;
        else if (wlan_state() != WL_OFF) hit = row_at(a, e->y);
    }
    if (e->type == EV_DOWN) st.pressed = hit;
    if (tap_track(&st.tap, e, dp(12)) && !st.sc.moved) {
        if (hit == HIT_SWITCH && wlan_available()) st.pending_power = wlan_state() == WL_OFF ? 1 : 2;
        else if (hit == HIT_SCAN) { st.note[0] = 0; net_lock(); wlan_scan(); net_unlock(); }
        else if (hit == HIT_DISCONNECT) { net_lock(); wlan_disconnect(); net_unlock(); }
        else if (hit == HIT_FORGET) { net_lock(); wlan_disconnect(); wlan_forget(); net_unlock(); }
        else if (hit == HIT_DIAG) { st.show_log = 1; st.sc.off = 0; st.note[0] = 0; }
        else if (hit == HIT_BACK) { st.show_log = 0; st.sc.off = 0; st.note[0] = 0; }
        else if (hit == HIT_SAVE) strlcpy(st.note, wifilog_save() ? "Saved as \\qrt\\hwdump\\wifi.txt" : "Could not save", sizeof st.note);
        else if (hit >= 0 && hit < st.n_nets && hit == st.pressed) connect_to(&st.nets[hit]);
    }
    if (e->type == EV_UP) st.pressed = -1;
    return 1;
}

static void on_open(void) {
    if (!wlan_available()) wifi_check_firmware();
    st.pressed = -1;
    st.sheet = 0;
    st.sc.off = 0;
    if (wlan_state() == WL_IDLE && !st.n_nets) { net_lock(); wlan_scan(); net_unlock(); }
}

static int tick(u64 now) {
    int redraw = 0;
    if (st.pending_power == 1) {
        static int shown;
        if (!shown) { shown = 1; return 1; }             /* let "Turning on..." reach the screen first */
        shown = 0;
        st.pending_power = 0;
        net_lock();
        if (wlan_power(1) == 0) wlan_scan();
        net_unlock();
        redraw = 1;
    } else if (st.pending_power == 2) {
        st.pending_power = 0;
        net_lock();
        wlan_power(0);
        net_unlock();
        st.n_nets = 0;
        redraw = 1;
    }
    if (now - st.last_refresh > 500) {
        st.last_refresh = now;
        int n = wlan_networks(st.nets, 32);
        if (n != st.n_nets) redraw = 1;
        st.n_nets = n;
        static char last_state[96];
        const char *s = wlan_state_text();
        if (strcmp(s, last_state)) { strlcpy(last_state, s, sizeof last_state); redraw = 1; }
        rect_t a = shell_app_area();
        if (st.show_log) {
            int lh = font_pick(F_MONO, dp(12))->line;
            st.sc.max = MAX(0, wifilog_count() * lh - (a.h - dp(100)));
            if (wifilog_serial() != st.log_seen) { st.log_seen = wifilog_serial(); st.sc.off = st.sc.max; redraw = 1; }
        } else st.sc.max = MAX(0, st.n_nets * ROW_H - list_rect(a).h);
    }
    return redraw;
}

const app_t app_wifi = { "Wi-Fi", "Wireless networks", RGB(0x21, 0x90, 0xa4), icon, on_open, draw, event, tick };
