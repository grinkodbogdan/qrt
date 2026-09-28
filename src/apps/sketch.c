/* Sketch: finger painting - the quickest way to see the touchscreen work. */
#include "../ui/shell.h"

static const u32 inks[] = {
    RGB(0xf5, 0xf3, 0xfa), RGB(0xff, 0x4f, 0xa3), RGB(0xff, 0x9f, 0x43),
    RGB(0xff, 0xd9, 0x3d), RGB(0x2e, 0xc4, 0xb6), RGB(0x4d, 0xa3, 0xff), RGB(0x7c, 0x6c, 0xff),
};
static const float sizes[] = { 3, 7, 14 };

static struct {
    canvas_t store;          /* max(W,H)^2, allocated on open: draw() may run on any core */
    int ink, size, drawing, blank;
    float lx, ly;
    tap_t tap;
} st = { .size = 1 };

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    gfx_line(c, cx - r * 0.6f, cy + r * 0.6f, cx + r * 0.55f, cy - r * 0.55f, r * 0.34f, fg);
    gfx_circle(c, cx - r * 0.72f, cy + r * 0.72f, r * 0.17f, fg);
}

static rect_t toolbar(rect_t a) { return (rect_t){ a.x + dp(16), a.y, a.w - dp(32), dp(56) }; }
static rect_t paper_rect(rect_t a) { return (rect_t){ a.x + dp(16), a.y + dp(68), a.w - dp(32), a.h - dp(80) }; }
static rect_t ink_rect(rect_t a, int i) { rect_t t = toolbar(a); return (rect_t){ t.x + i * dp(46), t.y + dp(6), dp(44), dp(44) }; }
static rect_t size_rect(rect_t a, int i) {
    rect_t t = toolbar(a);
    int x0 = t.x + (int)ARRAY_LEN(inks) * dp(46) + dp(12);
    return (rect_t){ x0 + i * dp(46), t.y + dp(6), dp(44), dp(44) };
}
static rect_t clear_rect(rect_t a) { rect_t t = toolbar(a); return (rect_t){ t.x + t.w - dp(96), t.y + dp(8), dp(96), dp(40) }; }

#define PAPER RGB(0x12, 0x10, 0x1c)

static void on_open(void) {
    int side = MAX(ui.W, ui.H);
    if (st.store.px && st.store.w >= side) return;
    canvas_free(&st.store);
    st.store = canvas_new(side, side);
    gfx_fill(&st.store, (rect_t){ 0, 0, side, side }, PAPER);
    st.blank = 1;
}

/* the paper is the top-left p.w x p.h window of the store */
static canvas_t paper(rect_t p) {
    canvas_t v = st.store;
    v.w = MIN(p.w, st.store.w);
    v.h = MIN(p.h, st.store.h);
    v.clip = v.limit = (rect_t){ 0, 0, v.w, v.h };
    return v;
}

static void draw(canvas_t *c, rect_t a) {
    rect_t p = paper_rect(a);
    for (int i = 0; i < (int)ARRAY_LEN(inks); i++) {
        rect_t r = ink_rect(a, i);
        float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
        if (i == st.ink) gfx_ring(c, cx, cy, dp(19), dp(2.5f), ui.text);
        gfx_circle(c, cx, cy, dp(14), inks[i]);
    }
    for (int i = 0; i < 3; i++) {
        rect_t r = size_rect(a, i);
        float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
        if (i == st.size) gfx_circle(c, cx, cy, dp(19), RGBA(255, 255, 255, 40));
        gfx_circle(c, cx, cy, dp(sizes[i]) / 2 + 1, ui.text);
    }
    rect_t cr = clear_rect(a);
    if (cr.x > size_rect(a, 2).x + dp(46)) ui_button(c, cr, "Clear", RGBA(255, 255, 255, 30), ui.text);

    gfx_shadow(c, p, dp(18), dp(12), RGBA(0, 0, 0, 80));
    if (st.store.px) {
        canvas_t pv = paper(p);
        gfx_blit_rounded(c, p.x, p.y, &pv, dp(16));
    }
    gfx_rrect_outline(c, p, dp(16), 1, ui.stroke);
    if (st.blank)
        gfx_text_center(c, ui.body, p, "Draw with your finger", ui.text3);
}

static int event(const event_t *e, rect_t a) {
    rect_t p = paper_rect(a);
    on_open();
    canvas_t pv = paper(p);
    if (e->type == EV_KEY && (e->ch == 'c' || e->ch == 'C')) goto clear;
    int pad = dp(sizes[st.size]) / 2 + 2;
    if (e->type == EV_DOWN && in_rect(p, e->x, e->y)) {
        int was_blank = st.blank;
        st.drawing = 1;
        st.blank = 0;
        st.lx = (float)(e->x - p.x); st.ly = (float)(e->y - p.y);
        gfx_circle(&pv, st.lx, st.ly, dp(sizes[st.size]) / 2.0f, inks[st.ink]);
        /* only the dot changed, unless the "draw with your finger" hint must go */
        if (!was_blank) shell_damage((rect_t){ e->x - pad, e->y - pad, 2 * pad, 2 * pad });
        return 1;
    }
    if (e->type == EV_MOVE && st.drawing) {
        float x = (float)(e->x - p.x), y = (float)(e->y - p.y);
        gfx_line(&pv, st.lx, st.ly, x, y, (float)dp(sizes[st.size]), inks[st.ink]);
        int x0 = (int)MIN(st.lx, x), y0 = (int)MIN(st.ly, y), x1 = (int)MAX(st.lx, x), y1 = (int)MAX(st.ly, y);
        shell_damage((rect_t){ p.x + x0 - pad, p.y + y0 - pad, x1 - x0 + 2 * pad + 1, y1 - y0 + 2 * pad + 1 });
        st.lx = x; st.ly = y;
        return 1;
    }
    if (e->type == EV_UP && st.drawing) { st.drawing = 0; st.tap.down = 0; return 0; }
    if (!tap_track(&st.tap, e, dp(12))) return 0;
    for (int i = 0; i < (int)ARRAY_LEN(inks); i++) if (in_rect(ink_rect(a, i), e->x, e->y)) { st.ink = i; return 1; }
    for (int i = 0; i < 3; i++) if (in_rect(size_rect(a, i), e->x, e->y)) { st.size = i; return 1; }
    if (in_rect(clear_rect(a), e->x, e->y)) goto clear;
    return 0;
clear:
    gfx_fill(&st.store, (rect_t){ 0, 0, st.store.w, st.store.h }, PAPER);
    st.blank = 1;
    return 1;
}

const app_t app_sketch = { "Sketch", "Draw with a finger", RGB(0xff, 0x4f, 0xa3), icon, on_open, draw, event, NULL };
