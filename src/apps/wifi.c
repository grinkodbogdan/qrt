/* Wi-Fi: turn the radio on, pick a network, type the password, see the log. */
#include "../ui/shell.h"
#include "../net/wlan.h"
#include "../net/net.h"
#include "../net/wifilog.h"
#include "../drivers/iwm/iwm.h"
#include "../net/netstack.h"

static struct {
    scroll_t sc;
    tap_t tap;
    int pressed;                 /* row or button under the finger */
    int show_log;
    int pending_power;           /* 1 on, 2 off: done in tick after a frame shows "starting" */
    int sheet;                   /* password sheet open */
    char sheet_ssid[33];
    char pass[64];
    int show_pass;
    char note[96];
    wlan_net_t nets[32];
    int n_nets;
    u32 log_seen;
    u64 last_refresh;
} st = { .pressed = -1 };

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
static rect_t switch_rect(rect_t a) { return (rect_t){ a.x + a.w - dp(24) - dp(64), a.y + dp(14), dp(64), dp(36) }; }
static rect_t btn_rect(rect_t a, int i) {       /* Scan, Disconnect, Forget, Log, Save log */
    int w = (a.w - dp(48) - 4 * dp(8)) / 5;
    return (rect_t){ a.x + dp(24) + i * (w + dp(8)), a.y + dp(88), w, dp(40) };
}
static rect_t list_rect(rect_t a) { int y = a.y + dp(144); return (rect_t){ a.x + dp(16), y, a.w - dp(32), a.y + a.h - y }; }
static int row_h(void) { return dp(58); }
static rect_t sheet_rect(rect_t a) { int w = MIN(a.w - dp(32), dp(520)); return (rect_t){ a.x + (a.w - w) / 2, a.y + dp(20), w, dp(236) }; }
static rect_t sheet_field(rect_t s) { return (rect_t){ s.x + dp(20), s.y + dp(70), s.w - dp(40), dp(48) }; }
static rect_t sheet_btn(rect_t s, int i) {       /* Show, Cancel, Connect */
    int w = (s.w - dp(40) - 2 * dp(10)) / 3;
    return (rect_t){ s.x + dp(20) + i * (w + dp(10)), s.y + dp(160), w, dp(48) };
}

static void bars(canvas_t *c, float x, float y, int rssi, u32 on, u32 off) {
    int level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int h = dp(5 + 4 * i);
        gfx_rrect(c, (rect_t){ (int)x + i * dp(7), (int)y - h, dp(5), h }, dp(1.5f), i < level ? on : off);
    }
}

static void draw(canvas_t *c, rect_t a) {
    int on = wlan_state() != WL_OFF;
    int avail = wlan_available();
    const char *state = st.pending_power == 1 ? "starting the radio..." : avail ? wlan_state_text() : "no supported wireless card";
    gfx_text_fit(c, ui.title, a.x + dp(24), a.y + dp(18), a.w - dp(140), state, ui.text);
    gfx_text_fit(c, ui.small, a.x + dp(24), a.y + dp(18) + ui.title->line + dp(2), a.w - dp(140),
                 avail ? "Intel Wireless 8260 \xc2\xb7 open and WPA2-Personal networks" : "", ui.text3);
    if (avail) {
        rect_t sw = switch_rect(a);
        int lit = on || st.pending_power == 1;
        gfx_rrect(c, sw, sw.h / 2, lit ? ui.accent : RGBA(255, 255, 255, 40));
        gfx_circle(c, lit ? sw.x + sw.w - sw.h / 2.0f : sw.x + sw.h / 2.0f, sw.y + sw.h / 2.0f, sw.h / 2.0f - dp(4), RGB(255, 255, 255));
    }
    static const char *btn[] = { "Scan", "Disconnect", "Forget", "Log", "Save log" };
    for (int i = 0; i < 5; i++) {
        int sel = i == 3 && st.show_log;
        ui_chip(c, btn_rect(a, i), btn[i], sel || st.pressed == 100 + i);
    }

    rect_t l = list_rect(a);
    gfx_clip(c, l);
    int y = l.y - st.sc.off;
    if (st.show_log) {
        const font_t *m = font_pick(F_MONO, dp(12));
        int n = wifilog_count();
        if (!n) gfx_text(c, ui.body, l.x + dp(8), y, "Nothing logged yet.", ui.text2);
        for (int i = 0; i < n; i++, y += m->line)
            if (y + m->line > l.y && y < l.y + l.h) gfx_text_fit(c, m, l.x + dp(8), y, l.w - dp(16), wifilog_line(i), RGB(0xd8, 0xf5, 0xd0));
    } else {
        if (st.note[0]) { gfx_text_fit(c, ui.small, l.x + dp(8), y, l.w - dp(16), st.note, ui.text2); y += ui.small->line + dp(6); }
        if (!avail) {
            static const char *lines[] = {
                "QRT drives the Intel Wireless 8260 of the Dell Venue 8 Pro 5855",
                "(a port of OpenBSD's iwm driver, firmware from linux-firmware).",
                "This machine has no such card.",
            };
            for (usize i = 0; i < ARRAY_LEN(lines); i++, y += ui.body->line) gfx_text_fit(c, ui.body, l.x + dp(8), y, l.w - dp(16), lines[i], ui.text2);
            const char *ns = net_status();
            if (ns) { y += dp(10); gfx_text_fit(c, ui.body, l.x + dp(8), y, l.w - dp(16), ns, ui.text); }
        } else if (!on) {
            gfx_text_fit(c, ui.body, l.x + dp(8), y, l.w - dp(16), "Turn Wi-Fi on to see networks.", ui.text2);
        } else {
            if (!st.n_nets) gfx_text_fit(c, ui.body, l.x + dp(8), y, l.w - dp(16),
                                         wlan_state() == WL_SCANNING ? "Looking for networks (about 5 seconds)..." : "No networks found yet. Tap Scan.", ui.text2);
            char saved[33] = { 0 };
            wlan_saved(saved, sizeof saved);
            for (int i = 0; i < st.n_nets; i++) {
                rect_t r = { l.x, y, l.w, row_h() - dp(4) };
                wlan_net_t *nw = &st.nets[i];
                if (st.pressed == i) gfx_rrect(c, r, dp(14), RGBA(255, 255, 255, 30));
                else gfx_rrect(c, r, dp(14), RGBA(255, 255, 255, 12));
                int ok = wlan_supported(nw->security);
                bars(c, r.x + dp(16), r.y + r.h / 2.0f + dp(9), nw->rssi, ok ? ui.text : ui.text3, RGBA(255, 255, 255, 40));
                gfx_text_fit(c, ui.label, r.x + dp(60), r.y + dp(8), r.w - dp(76), nw->ssid, ok ? ui.text : ui.text3);
                char sub[96];
                fmt(sub, sizeof sub, "%s%s \xc2\xb7 channel %d \xc2\xb7 %d dBm%s", wlan_security_name(nw->security),
                    ok ? "" : " (not supported)", nw->channel, nw->rssi, !strcmp(saved, nw->ssid) ? " \xc2\xb7 saved" : "");
                gfx_text_fit(c, ui.small, r.x + dp(60), r.y + dp(8) + ui.label->line, r.w - dp(76), sub, ui.text2);
                y += row_h();
            }
        }
    }
    gfx_unclip(c);

    if (st.sheet) {
        gfx_fill(c, a, RGBA(0, 0, 0, 120));
        rect_t s = sheet_rect(a);
        ui_card(c, s, dp(20), 1);
        char t[64];
        fmt(t, sizeof t, "Password for %s", st.sheet_ssid);
        gfx_text_fit(c, ui.title, s.x + dp(20), s.y + dp(20), s.w - dp(40), t, ui.text);
        rect_t f = sheet_field(s);
        gfx_rrect(c, f, dp(12), RGBA(0, 0, 0, 90));
        char shown[64];
        usize n = strlen(st.pass);
        if (st.show_pass) strlcpy(shown, st.pass, sizeof shown);
        else { usize i; for (i = 0; i < n && i < 40; i++) shown[i] = '*'; shown[i] = 0; }
        const font_t *m = font_pick(F_MONO, dp(16));
        int tx = gfx_text(c, m, f.x + dp(12), f.y + (f.h - m->line) / 2, shown, ui.text);
        gfx_fill(c, (rect_t){ tx + 1, f.y + dp(10), dp(2), f.h - dp(20) }, ui.accent);
        gfx_text_fit(c, ui.small, f.x, f.y + f.h + dp(8), f.w, n && n < 8 ? "WPA2 passwords have at least 8 characters" : "The key is saved in NVRAM for reconnecting",
                     ui.text3);
        static const char *sb[] = { "Show", "Cancel", "Connect" };
        for (int i = 0; i < 3; i++)
            ui_button(c, sheet_btn(s, i), i == 0 && st.show_pass ? "Hide" : sb[i],
                      i == 2 ? ui.accent : RGBA(255, 255, 255, st.pressed == 200 + i ? 60 : 30), ui.text);
    }
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
        fmt(st.note, sizeof st.note, "%s: %s networks are not supported yet.", nw->ssid, wlan_security_name(nw->security));
        return;
    }
    char saved[33];
    if (nw->security == SEC_OPEN || (wlan_saved(saved, sizeof saved) && !strcmp(saved, nw->ssid))) { net_lock(); wlan_connect(nw->ssid, NULL); net_unlock(); }
    else open_sheet(nw->ssid);
}

static int event(const event_t *e, rect_t a) {
    if (st.sheet) {
        rect_t s = sheet_rect(a);
        if (e->type == EV_KEY) {
            usize n = strlen(st.pass);
            if (e->scan == SCAN_ESC) { st.sheet = 0; shell_keyboard(0); }
            else if (e->ch == '\r') { if (n >= 8) { st.sheet = 0; shell_keyboard(0); { net_lock(); wlan_connect(st.sheet_ssid, st.pass); net_unlock(); } } }
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
            else if (hit == 202 && strlen(st.pass) >= 8) { st.sheet = 0; shell_keyboard(0); { net_lock(); wlan_connect(st.sheet_ssid, st.pass); net_unlock(); } }
            else if (in_rect(sheet_field(s), e->x, e->y)) shell_keyboard(1);
        }
        if (e->type == EV_UP) st.pressed = -1;
        return 1;
    }
    if (e->type == EV_KEY) return scroll_event(&st.sc, e, list_rect(a), dp(40));
    rect_t l = list_rect(a);
    if (scroll_event(&st.sc, e, l, dp(40))) { st.pressed = -1; return 1; }
    int hit = -1;
    for (int i = 0; i < 5; i++) if (in_rect(btn_rect(a, i), e->x, e->y)) hit = 100 + i;
    if (in_rect(switch_rect(a), e->x, e->y)) hit = 150;
    if (!st.show_log && in_rect(l, e->x, e->y) && wlan_state() != WL_OFF) {
        int y0 = l.y - st.sc.off + (st.note[0] ? ui.small->line + dp(6) : 0);
        int r = (e->y - y0) / row_h();
        if (e->y >= y0 && r >= 0 && r < st.n_nets) hit = r;
    }
    if (e->type == EV_DOWN) st.pressed = hit;
    if (tap_track(&st.tap, e, dp(12)) && !st.sc.moved) {
        if (hit == 150 && wlan_available()) st.pending_power = wlan_state() == WL_OFF ? 1 : 2;
        else if (hit == 100) { st.note[0] = 0; net_lock(); int sr = wlan_scan(); net_unlock(); if (sr) strlcpy(st.note, "Turn Wi-Fi on first (or wait for the connection attempt).", sizeof st.note); }
        else if (hit == 101) { net_lock(); wlan_disconnect(); net_unlock(); }
        else if (hit == 102) { { net_lock(); wlan_forget(); net_unlock(); } strlcpy(st.note, "Saved network forgotten.", sizeof st.note); }
        else if (hit == 103) { st.show_log = !st.show_log; st.sc.off = 0; }
        else if (hit == 104) strlcpy(st.note, wifilog_save() ? "Log saved to \\qrt\\hwdump\\wifi.txt" : "Could not save the log", sizeof st.note);
        else if (hit >= 0 && hit < st.n_nets && hit == st.pressed) connect_to(&st.nets[hit]);
    }
    if (e->type == EV_UP) st.pressed = -1;
    return 1;
}

static void on_open(void) {
    if (!wlan_available()) iwm_check_firmware();
    st.pressed = -1;
    st.sheet = 0;
    st.sc.off = 0;
    if (wlan_state() == WL_IDLE && !st.n_nets) { net_lock(); wlan_scan(); net_unlock(); }
}

static int tick(u64 now) {
    int redraw = 0;
    if (st.pending_power == 1) {
        static int shown;
        if (!shown) { shown = 1; return 1; }             /* let "starting..." reach the screen first */
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
        int rows = st.show_log ? wifilog_count() : st.n_nets;
        int rh = st.show_log ? font_pick(F_MONO, dp(12))->line : row_h();
        st.sc.max = rows * rh - list_rect(shell_app_area()).h + dp(20);
        if (st.show_log && wifilog_serial() != st.log_seen) {
            st.log_seen = wifilog_serial();
            st.sc.off = MAX(0, st.sc.max);                /* follow the tail */
            redraw = 1;
        }
    }
    return redraw;
}

const app_t app_wifi = { "Wi-Fi", "Networks", RGB(0x2e, 0x8b, 0xff), icon, on_open, draw, event, tick };
