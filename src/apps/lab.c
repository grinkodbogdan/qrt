/* Touch Lab: probe and switch to QRT's own touchscreen driver. */
#include "../ui/shell.h"
#include "../drivers/touch.h"

static struct { scroll_t sc; tap_t tap; char note[64]; int tx, ty, tdown; } st;

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    gfx_ring(c, cx, cy, r * 0.8f, r * 0.2f, fg);
    gfx_circle(c, cx, cy, r * 0.28f, fg);
    gfx_line(c, cx, cy - r, cx, cy - r * 0.55f, r * 0.18f, fg);
    gfx_line(c, cx, cy + r * 0.55f, cx, cy + r, r * 0.18f, fg);
    gfx_line(c, cx - r, cy, cx - r * 0.55f, cy, r * 0.18f, fg);
    gfx_line(c, cx + r * 0.55f, cy, cx + r, cy, r * 0.18f, fg);
}

static void on_open(void) {
    if (!nt.probed) ntouch_probe();
    st.note[0] = 0;
}

static rect_t btn(rect_t a, int i) {
    int w = (a.w - dp(32) - 2 * dp(10)) / 3;
    return (rect_t){ a.x + dp(16) + i * (w + dp(10)), a.y, w, dp(48) };
}

typedef struct { canvas_t *c; rect_t col; int y; } flow_t;
static void line(flow_t *f, const char *key, const char *val) {
    rect_t r = { f->col.x, f->y, f->col.w, ui.body->line };
    if (f->y + r.h > f->c->clip.y && f->y < f->c->clip.y + f->c->clip.h) ui_kv(f->c, r, key, val);
    f->y += ui.body->line + dp(2);
}
static void head(flow_t *f, const char *t) {
    f->y += dp(12);
    ui_section(f->c, f->col.x, f->y, t);
    f->y += ui.small->line + dp(4);
}

static void draw(canvas_t *c, rect_t a) {
    ui_button(c, btn(a, 0), "Probe", RGBA(255, 255, 255, 30), ui.text);
    ui_button(c, btn(a, 1), nt.active ? "Firmware touch" : "Go native", nt.active ? RGBA(255, 255, 255, 30) : ui.accent, ui.text);
    ui_button(c, btn(a, 2), "Save report", RGBA(255, 255, 255, 30), ui.text);

    rect_t card = { a.x + dp(16), a.y + dp(60), a.w - dp(32), a.h - dp(68) };
    ui_card(c, card, dp(22), 0);
    rect_t in = { card.x + dp(22), card.y + dp(10), card.w - dp(44), card.h - dp(20) };
    gfx_clip(c, in);
    flow_t f = { c, in, in.y - st.sc.off };
    char b[128];

    rect_t banner = { in.x, f.y + dp(8), in.w, dp(44) };
    gfx_rrect(c, banner, dp(12), nt.active ? ALPHA(ui.accent, 90) : RGBA(255, 255, 255, 18));
    gfx_text_fit(c, ui.label, banner.x + dp(14), banner.y + (banner.h - ui.label->line) / 2, banner.w - dp(28),
                 nt.status[0] ? nt.status : "not probed", ui.text);
    f.y += dp(56);
    if (st.note[0]) { gfx_text(c, ui.small, in.x, f.y, st.note, ui.text2); f.y += ui.small->line; }

    head(&f, "I2C CONTROLLER (NATIVE DESIGNWARE DRIVER)");
    line(&f, "Bus", "I2C6  \xc2\xb7  PCI 00:18.6");
    if (nt.bus.found) {
        fmt(b, sizeof b, "%04x:%04x, DW_apb_i2c v%c.%c%c", nt.bus.pci_id & 0xffff, nt.bus.pci_id >> 16,
            (char)(nt.bus.comp_ver >> 24), (char)(nt.bus.comp_ver >> 16), (char)(nt.bus.comp_ver >> 8));
        line(&f, "Controller", b);
        fmt(b, sizeof b, "TX %u, RX %u entries", nt.bus.tx_depth, nt.bus.rx_depth);
        line(&f, "FIFOs", b);
    } else line(&f, "Controller", dwi2c_strerror(nt.bus_err));
    if (nt.board_valid) {
        fmt(b, sizeof b, "BDID %u, MPNL %u, ITSA 0x%02x, WLID %u", nt.bdid, nt.mpnl, nt.itsa, nt.wlid);
        line(&f, "Board IDs", b);
    }

    for (int i = 0; i < TOUCH_CANDIDATES; i++) {
        touch_candidate_t *t = &nt.cand[i];
        fmt(b, sizeof b, "%s  \xc2\xb7  0x%02X", t->name ? t->name : "?", t->addr);
        head(&f, b);
        if (t->result) {
            fmt(b, sizeof b, "%s", t->result == 1 ? "not tried" : dwi2c_strerror(t->result));
            line(&f, "Result", b);
            continue;
        }
        fmt(b, sizeof b, "%04x:%04x  version %04x", t->hid.d.vid, t->hid.d.pid, t->hid.d.ver);
        line(&f, "HID device", b);
        fmt(b, sizeof b, "%u bytes, %d fields", t->hid.d.rdesc_len, t->hid.nf);
        line(&f, "Report desc", b);
        fmt(b, sizeof b, "%d fingers, X %d..%d, Y %d..%d", t->hid.nfingers, t->hid.xmin, t->hid.xmax, t->hid.ymin, t->hid.ymax);
        line(&f, "Touch", t->hid.nfingers ? b : "no finger collections (pen or other device)");
    }

    if (nt.primary >= 0) {
        i2chid_t *h = &nt.cand[nt.primary].hid;
        head(&f, "LIVE");
        fmt(b, sizeof b, "%u reports, %u empty reads, %u errors", h->reports, h->empty_reads, h->errors);
        line(&f, "Native reads", b);
        fmt(b, sizeof b, "%s  x %d  y %d  (id %d)", h->down ? "DOWN" : "up", h->x, h->y, h->track_id);
        line(&f, "Primary contact", b);
        char hex[3 * 24 + 1];
        int o = 0;
        for (int j = 0; j < nt.last_len && j < 24; j++) o += fmt(hex + o, sizeof hex - o, "%02x ", nt.last[j]);
        hex[o] = 0;
        line(&f, "Last report", nt.last_len ? hex : "-");
    }
    f.y += dp(16);
    st.sc.max = f.y + st.sc.off - in.y - in.h;
    gfx_unclip(c);
    gfx_clip(c, a);
    if (nt.active && st.tdown) {                     /* crosshair under the finger */
        gfx_ring(c, (float)st.tx, (float)st.ty, dp(26), dp(3), ui.accent);
        gfx_circle(c, (float)st.tx, (float)st.ty, dp(5), ui.accent);
    }
}

static int event(const event_t *e, rect_t a) {
    if (e->type == EV_DOWN || e->type == EV_MOVE) { st.tx = e->x; st.ty = e->y; st.tdown = 1; }
    if (e->type == EV_UP) st.tdown = 0;
    int redraw = scroll_event(&st.sc, e, (rect_t){ a.x, a.y + dp(60), a.w, a.h - dp(60) }, dp(48)) || nt.active;
    if (!tap_track(&st.tap, e, dp(12))) return redraw;
    if (in_rect(btn(a, 0), e->x, e->y)) {
        if (nt.active) strlcpy(st.note, "switch back to firmware touch before probing", sizeof st.note);
        else { ntouch_probe(); st.note[0] = 0; }
    } else if (in_rect(btn(a, 1), e->x, e->y)) {
        if (nt.active) { ntouch_revert(); strlcpy(nt.status, "firmware touch restored", sizeof nt.status); }
        else ntouch_go_native();
    } else if (in_rect(btn(a, 2), e->x, e->y)) {
        strlcpy(st.note, ntouch_save() ? "saved \\qrt\\hwdump\\touch.txt" : "could not write to the stick", sizeof st.note);
    }
    return 1;
}

static int tick(u64 now) {
    static u64 last;
    if (now - last < 250) return 0;
    last = now;
    return nt.active;
}

const app_t app_lab = { "Touch Lab", "Native touch driver", RGB(0xe8, 0x5d, 0x75), icon, on_open, draw, event, tick };
