/* Bluetooth: in the style of GNOME's Bluetooth panel - the adapter, a scan
 * button and the devices around.  Headphones and speakers (A2DP) have a Connect
 * button: QRT pairs with them ("just works", as headphones do), keeps the link key,
 * and plays all sound on them while they are connected (src/drivers/bt/a2dp.c). */
#include "../ui/shell.h"
#include "../drivers/bt/bt.h"

static struct {
    scroll_t sc;
    tap_t tap;
    bt_device_t dev[BT_MAX_DEV];
    int n, state;
    char status[112], audio[96];
    u64 last;
} st;

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    float w = r * 0.42f, h = r * 0.8f, t = r * 0.14f;
    gfx_line(c, cx, cy - h, cx, cy + h, t, fg);                       /* the rune: a stem and two loops */
    gfx_line(c, cx, cy - h, cx + w, cy - h / 2, t, fg);
    gfx_line(c, cx + w, cy - h / 2, cx - w, cy + h / 2, t, fg);
    gfx_line(c, cx - w, cy - h / 2, cx + w, cy + h / 2, t, fg);
    gfx_line(c, cx + w, cy + h / 2, cx, cy + h, t, fg);
}

#define ROW_H dp(60)
static int col_w(rect_t a) { return MIN(a.w - dp(48), dp(680)); }
static int col_x(rect_t a) { return a.x + (a.w - col_w(a)) / 2; }
static rect_t top_group(rect_t a) { return (rect_t){ col_x(a), a.y + dp(24), col_w(a), ROW_H }; }
static rect_t scan_btn(rect_t a) { rect_t g = top_group(a); int w = dp(110); return (rect_t){ g.x + g.w - dp(14) - w, g.y + dp(12), w, ROW_H - dp(24) }; }
static rect_t list_rect(rect_t a) { rect_t g = top_group(a); int y = g.y + g.h + dp(24) + ui.label->line + dp(10) + (st.audio[0] ? ui.small->line + dp(8) : 0); return (rect_t){ g.x, y, g.w, a.y + a.h - y - dp(16) }; }
static rect_t row_rect(rect_t a, int i) { rect_t l = list_rect(a); return (rect_t){ l.x, l.y - st.sc.off + i * ROW_H, l.w, ROW_H }; }
static rect_t conn_btn(rect_t r) { int w = dp(124); return (rect_t){ r.x + r.w - dp(14) - w, r.y + dp(12), w, ROW_H - dp(24) }; }
/* classic devices that may take audio: headphones, speakers, anything paired or unclassified */
static int can_audio(const bt_device_t *d) { return !d->le && (d->paired || !d->cod || ((d->cod >> 8) & 0x1f) == 0x04); }

static void refresh(void) {
    st.state = bt_state();
    strlcpy(st.status, bt_status(), sizeof st.status);
    strlcpy(st.audio, bt_audio_status(), sizeof st.audio);
    st.n = bt_devices(st.dev, BT_MAX_DEV);
}

static void draw(canvas_t *c, rect_t a) {
    rect_t g = top_group(a);
    gfx_rrect(c, g, dp(12), RGBA(255, 255, 255, 12));
    const char *title = st.state == BT_NONE ? "No Bluetooth adapter" : st.state == BT_FAILED ? "Bluetooth is not working" :
                        st.state == BT_STARTING ? "Bluetooth is starting" : "Bluetooth";
    gfx_text(c, ui.body, g.x + dp(16), g.y + ROW_H / 2 - ui.body->line + dp(2), title, ui.text);
    gfx_text_fit(c, ui.small, g.x + dp(16), g.y + ROW_H / 2 + dp(2), g.w - dp(160), st.status, ui.text2);
    if (st.state == BT_READY || st.state == BT_SCANNING)
        ui_button(c, scan_btn(a), st.state == BT_SCANNING ? "Scanning..." : "Scan", st.state == BT_SCANNING ? RGBA(255, 255, 255, 22) : ui.accent, ui.text);

    rect_t l = list_rect(a);
    if (st.audio[0]) gfx_text_fit(c, ui.small, g.x + dp(4), g.y + g.h + dp(10), g.w - dp(8), st.audio, ui.accent);
    gfx_text(c, ui.label, l.x + dp(4), l.y - ui.label->line - dp(10), "Devices", ui.text);
    rect_t old = c->clip;
    gfx_clip(c, l);
    if (!st.n) {
        const char *msg = st.state == BT_SCANNING ? "Looking for devices nearby..." :
                          st.state == BT_READY ? "Tap Scan to look for devices nearby" : "";
        gfx_text(c, ui.body, l.x + dp(4), l.y + dp(8), msg, ui.text2);
    } else {
        rect_t box = { l.x, l.y - st.sc.off, l.w, st.n * ROW_H };
        gfx_rrect(c, box, dp(12), RGBA(255, 255, 255, 12));
        for (int i = 0; i < st.n; i++) {
            bt_device_t *d = &st.dev[i];
            rect_t r = { box.x, box.y + i * ROW_H, box.w, ROW_H };
            if (r.y + r.h < l.y || r.y > l.y + l.h) continue;
            if (i) gfx_fill(c, (rect_t){ r.x, r.y, r.w, 1 }, RGBA(0, 0, 0, 80));
            char nm[64], sub[80];
            if (d->named) strlcpy(nm, d->name, sizeof nm);
            else fmt(nm, sizeof nm, "%02X:%02X:%02X:%02X:%02X:%02X", d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0]);
            int on = bt_audio_connected(d->addr);
            if (on) strlcpy(sub, "Connected: QRT's sound plays here", sizeof sub);
            else if (d->rssi) fmt(sub, sizeof sub, "%s%s, signal %d dBm", bt_kind(d->cod, d->le), d->paired ? ", paired" : "", d->rssi);
            else fmt(sub, sizeof sub, "%s%s", bt_kind(d->cod, d->le), d->paired ? ", paired" : "");
            int tw = r.w - dp(32) - (can_audio(d) ? dp(140) : 0);
            gfx_text_fit(c, ui.body, r.x + dp(16), r.y + ROW_H / 2 - ui.body->line + dp(2), tw, nm, ui.text);
            gfx_text_fit(c, ui.small, r.x + dp(16), r.y + ROW_H / 2 + dp(2), tw, sub, on ? ui.accent : ui.text2);
            if (can_audio(d)) ui_button(c, conn_btn(r), on ? "Disconnect" : "Connect", on ? RGBA(255, 255, 255, 22) : ui.accent, ui.text);
        }
    }
    st.sc.max = MAX(0, st.n * ROW_H - l.h);
    c->clip = old;
}

static int event(const event_t *e, rect_t a) {
    int scrolled = scroll_event(&st.sc, e, list_rect(a), dp(48));
    if (!tap_track(&st.tap, e, dp(12))) return scrolled;
    if (in_rect(scan_btn(a), e->x, e->y) && st.state == BT_READY) { bt_scan(); refresh(); st.state = BT_SCANNING; return 1; }
    rect_t l = list_rect(a);
    for (int i = 0; i < st.n; i++) {
        rect_t b = conn_btn(row_rect(a, i));
        if (!can_audio(&st.dev[i]) || !in_rect(b, e->x, e->y) || !in_rect(l, e->x, e->y)) continue;
        if (bt_audio_connected(st.dev[i].addr)) bt_audio_disconnect();
        else bt_audio_connect(st.dev[i].addr);
        refresh();
        return 1;
    }
    return scrolled;
}

static int tick(u64 now) {
    if (now - st.last < 500) return 0;
    st.last = now;
    int old_state = st.state, old_n = st.n;
    char old_status[112], old_audio[96];
    strlcpy(old_status, st.status, sizeof old_status);
    strlcpy(old_audio, st.audio, sizeof old_audio);
    refresh();
    return st.state != old_state || st.n != old_n || strcmp(st.status, old_status) || strcmp(st.audio, old_audio) || st.state == BT_SCANNING;
}

static void on_open(void) { st.sc.off = 0; refresh(); }

const app_t app_bluetooth = { "Bluetooth", "Headphones, speakers, devices nearby", RGB(0x1c, 0x71, 0xd8), icon, on_open, draw, event, tick, NULL };
