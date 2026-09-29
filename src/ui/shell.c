/*
 * shell.c - QRT's user interface.
 *
 * Visual language borrows from Fuchsia's Armadillo/Ermine shells: a
 * wallpaper of soft light, glassy "story" cards and a big quiet clock, with
 * a floating dock that can be dragged to any screen edge, a searchable app
 * launcher and an on-screen keyboard.  Everything is drawn in software into
 * a logical canvas which is rotated (for tablets held in either
 * orientation) and pushed to the panel through GOP.
 */
#include "shell.h"
#include "../kernel/smp.h"
#include "osk.h"
#include "../net/netstack.h"
#include "../drivers/backlight.h"
#include "../drivers/buttons.h"

ui_t ui;

const u32 accent_palette[N_ACCENTS] = {
    RGB(0xff, 0x4f, 0xa3), RGB(0x7c, 0x6c, 0xff), RGB(0x2e, 0xc4, 0xb6),
    RGB(0xff, 0x8a, 0x4c), RGB(0x4d, 0xa3, 0xff), RGB(0xa6, 0xe2, 0x2e),
};
const char *accent_names[N_ACCENTS] = { "Fuchsia", "Iris", "Lagoon", "Ember", "Sky", "Lime" };

static const app_t *apps[] = { &app_files, &app_terminal, &app_browser, &app_wifi, &app_sketch, &app_settings, &app_system, &app_clock, &app_life, &app_lab };
#define N_APPS ((int)ARRAY_LEN(apps))

typedef enum { VIEW_HOME, VIEW_APP } view_t;

static struct {
    canvas_t scene, frame, phys, wall, wall_dim;
    int rot, accent_idx;
    view_t view;
    const app_t *app;
    int dirty;
    tap_t tap;
    int launcher_open, launch_pressed, dock_pressed, power_open, owner, keyboard_pending;
    int dock_edge;                /* DOCK_RIGHT/LEFT/BOTTOM/TOP, saved as QrtDockEdge */
    int dock_drag, drag_x, drag_y, drag_x0, drag_y0;    /* the dock following a finger */
    int grab_fx, grab_fy, grab_vert;  /* where the finger holds the dock (per mille of its size) */
    int animating; u64 anim_t0; rect_t anim_from, anim_to, anim_rect;   /* snapping to an edge */
    int locked, asleep, lock_dragging, lock_y0, lock_armed;
    u64 last_input_ms, now_ms;
    int sleep_after;              /* seconds without input before sleeping; 0 = never (QrtSleepAfter) */
    int volume, volume_saved;     /* mock audio: 0..100 (QrtVolume) */
    u64 osd_until;
    int osd_shown;
    u32 running;                  /* bit i: apps[i] was opened */
    char query[48];
    int cursor_x, cursor_y, cursor_on, cursor_dirty;
    rect_t dmg;          /* content that must be recomposed */
    rect_t pdmg;         /* extra area to push to the panel (cursor moves) */
    int app_damaged, bench_pending;
    rect_t cursor_drawn;
    int last_minute;
} sh;

shell_stats_t shell_stats;

void shell_redraw(void) { sh.dirty = 1; }
void shell_damage(rect_t r) { sh.dmg = rect_union(sh.dmg, r); sh.app_damaged = 1; }
static rect_t full_rect(void) { return (rect_t){ 0, 0, ui.W, ui.H }; }
int  shell_rotation(void) { return sh.rot; }
int  shell_accent_index(void) { return sh.accent_idx; }

/* ---- widgets -------------------------------------------------------------- */
int tap_track(tap_t *t, const event_t *e, int slop) {
    switch (e->type) {
    case EV_DOWN: t->down = 1; t->moved = 0; t->x0 = e->x; t->y0 = e->y; return 0;
    case EV_MOVE:
        if (t->down && (ABS_I(e->x - t->x0) > slop || ABS_I(e->y - t->y0) > slop)) t->moved = 1;
        return 0;
    case EV_UP: {
        int tap = t->down && !t->moved;
        t->down = 0;
        return tap;
    }
    default: return 0;
    }
}

int scroll_event(scroll_t *s, const event_t *e, rect_t area, int step) {
    int old = s->off;
    if (e->type == EV_DOWN && in_rect(area, e->x, e->y)) { s->dragging = 1; s->last_y = e->y; s->moved = 0; }
    else if (e->type == EV_MOVE && s->dragging) {
        s->off -= e->y - s->last_y;
        if (ABS_I(e->y - s->last_y) > 0) s->moved += ABS_I(e->y - s->last_y);
        s->last_y = e->y;
    } else if (e->type == EV_UP) s->dragging = 0;
    else if (e->type == EV_SCROLL) s->off += e->dy * step;
    else if (e->type == EV_KEY && e->scan == SCAN_DOWN) s->off += step;
    else if (e->type == EV_KEY && e->scan == SCAN_UP) s->off -= step;
    else if (e->type == EV_KEY && e->scan == SCAN_PGDN) s->off += area.h * 3 / 4;
    else if (e->type == EV_KEY && e->scan == SCAN_PGUP) s->off -= area.h * 3 / 4;
    s->off = CLAMP(s->off, 0, MAX(0, s->max));
    return s->off != old;
}

void ui_card(canvas_t *c, rect_t r, int radius, int hi) {
    gfx_shadow(c, r, radius, dp(14), RGBA(0, 0, 0, 70));
    gfx_rrect(c, r, radius, hi ? ui.card_hi : ui.card);
    gfx_rrect_outline(c, r, radius, 1, ui.stroke);
}

void ui_button(canvas_t *c, rect_t r, const char *label, u32 fill, u32 fg) {
    gfx_rrect(c, r, r.h / 2, fill);
    gfx_text_center(c, ui.label, r, label, fg);
}

void ui_chip(canvas_t *c, rect_t r, const char *label, int selected) {
    if (selected) gfx_rrect(c, r, r.h / 2, ui.accent);
    else { gfx_rrect(c, r, r.h / 2, RGBA(255, 255, 255, 22)); gfx_rrect_outline(c, r, r.h / 2, 1, ui.stroke); }
    gfx_text_center(c, ui.label, r, label, selected ? RGB(255, 255, 255) : ui.text);
}

void ui_section(canvas_t *c, int x, int y, const char *title) {
    gfx_text(c, ui.small, x, y, title, ui.text3);
}

void ui_kv(canvas_t *c, rect_t r, const char *key, const char *value) {
    int kw = MIN(r.w * 2 / 5, dp(150));
    gfx_text_fit(c, ui.body, r.x, r.y, kw - dp(8), key, ui.text2);
    gfx_text_fit(c, ui.body, r.x + kw, r.y, r.w - kw, value, ui.text);
}

/* ---- metrics & theme ------------------------------------------------------ */
static void ui_metrics(void) {
    ui.W = (sh.rot & 1) ? (int)k.fb_h : (int)k.fb_w;
    ui.H = (sh.rot & 1) ? (int)k.fb_w : (int)k.fb_h;
    ui.landscape = ui.W > ui.H;
    /* An 8" tablet is ~680 dp across its short edge. */
    float s = (float)MIN(ui.W, ui.H) / 680.0f;
    ui.s = CLAMP(s, 1.0f, 3.0f);
    ui.small   = font_pick(F_REGULAR, dp(13));
    ui.body    = font_pick(F_REGULAR, dp(15));
    ui.label   = font_pick(F_SEMIBOLD, dp(15));
    ui.title   = font_pick(F_SEMIBOLD, dp(20));
    ui.h1      = font_pick(F_SEMIBOLD, dp(28));
    ui.display = font_pick(F_LIGHT, dp(56));
    ui.huge    = font_pick(F_LIGHT, dp(ui.landscape ? 96 : 120));
    ui.accent  = accent_palette[sh.accent_idx];
    ui.text    = RGB(0xf5, 0xf3, 0xfa);
    ui.text2   = RGBA(0xf5, 0xf3, 0xfa, 170);
    ui.text3   = RGBA(0xf5, 0xf3, 0xfa, 110);
    ui.card    = RGBA(0x1c, 0x18, 0x2c, 170);
    ui.card_hi = RGBA(0x3a, 0x33, 0x55, 200);
    ui.stroke  = RGBA(255, 255, 255, 30);
    ui.bg_top    = RGB(0x14, 0x0f, 0x26);
    ui.bg_bottom = RGB(0x06, 0x0b, 0x19);
}

/* Soft radial light, the signature of the wallpaper. */
static void glow(canvas_t *c, float cx, float cy, float rad, u32 color) {
    rect_t cl = c->clip;
    int x0 = MAX(cl.x, (int)(cx - rad)), x1 = MIN(cl.x + cl.w, (int)(cx + rad));
    int y0 = MAX(cl.y, (int)(cy - rad)), y1 = MIN(cl.y + cl.h, (int)(cy + rad));
    u32 a = color >> 24;
    /* coarse 4x4 evaluation keeps boot fast on an Atom; the falloff is smooth
     * enough that bilinear-free block shading plus dithering is invisible */
    for (int y = y0; y < y1; y++) {
        u32 *row = c->px + (usize)y * c->stride;
        float dy = (y - cy) / rad;
        for (int x = x0; x < x1; x++) {
            float dx = (x - cx) / rad;
            float d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            float t = 1 - d2;
            u32 al = (u32)(t * t * a) + ((x ^ y) & 1);
            u32 dst = row[x];
            u32 rb = ((color & 0xff00ff) * al + (dst & 0xff00ff) * (255 - al)) >> 8;
            u32 g  = ((color & 0x00ff00) * al + (dst & 0x00ff00) * (255 - al)) >> 8;
            row[x] = 0xff000000u | (rb & 0xff00ff) | (g & 0x00ff00);
        }
    }
}

/* Horizontal band i of n within rectangle r. */
static rect_t band(rect_t r, int i, int n) {
    int y0 = r.y + r.h * i / n, y1 = r.y + r.h * (i + 1) / n;
    return (rect_t){ r.x, y0, r.w, y1 - y0 };
}

/* One band of the wallpaper; runs on any core (pure pixel work). */
static void wallpaper_job(void *arg, int i, int n) {
    rect_t b = band(full_rect(), i, n);
    canvas_t w = sh.wall, dim = sh.wall_dim;       /* private clip state per core */
    gfx_limit(&w, b);
    gfx_limit(&dim, b);
    float W = (float)ui.W, H = (float)ui.H, m = (float)MAX(ui.W, ui.H);
    gfx_vgradient(&w, full_rect(), ui.bg_top, ui.bg_bottom);
    glow(&w, W * 0.85f, H * 0.10f, m * 0.55f, ALPHA(ui.accent, 120));
    glow(&w, W * 0.05f, H * 0.55f, m * 0.50f, RGBA(0x5b, 0x3c, 0xff, 90));
    glow(&w, W * 0.70f, H * 1.00f, m * 0.45f, RGBA(0x00, 0xb3, 0xc6, 70));
    memcpy(dim.px + (usize)b.y * ui.W, w.px + (usize)b.y * ui.W, (usize)b.h * ui.W * 4);
    gfx_fill(&dim, b, RGBA(4, 3, 10, 120));
}

static void build_wallpaper(void) { smp_run(wallpaper_job, NULL, MAX(4, smp_workers())); }

static void alloc_canvases(void) {
    canvas_free(&sh.scene); canvas_free(&sh.frame); canvas_free(&sh.wall); canvas_free(&sh.wall_dim);
    sh.scene = canvas_new(ui.W, ui.H);
    sh.frame = canvas_new(ui.W, ui.H);
    sh.wall = canvas_new(ui.W, ui.H);
    sh.wall_dim = canvas_new(ui.W, ui.H);
    if (!sh.phys.px) sh.phys = canvas_new((int)k.fb_w, (int)k.fb_h);
}

static void relayout(void) {
    ui_metrics();
    alloc_canvases();
    build_wallpaper();
    sh.dirty = 1;
}

void shell_set_rotation(int rot) {
    sh.rot = rot & 3;
    hal_setting_set(u"QrtRotation", (u32)sh.rot);
    relayout();
}

void shell_set_accent(int idx) {
    sh.accent_idx = CLAMP(idx, 0, N_ACCENTS - 1);
    hal_setting_set(u"QrtAccent", (u32)sh.accent_idx);
    relayout();
}

/* ---- presentation --------------------------------------------------------- */
static void to_logical(int px, int py, int *lx, int *ly) {
    int fw = (int)k.fb_w, fh = (int)k.fb_h;
    switch (sh.rot) {
    case 1:  *lx = py;          *ly = fw - 1 - px; break;
    case 2:  *lx = fw - 1 - px; *ly = fh - 1 - py; break;
    case 3:  *lx = fh - 1 - py; *ly = px;          break;
    default: *lx = px;          *ly = py;          break;
    }
}

/* Push rectangle d (logical coords) of src to the panel, rotating on the way. */
static void present(const canvas_t *src, rect_t d) {
    d = rect_intersect(d, full_rect());
    if (d.w <= 0 || d.h <= 0) return;
    if (!sh.rot) {
        hal_present(src->px, src->stride, d.x, d.y, d.w, d.h);
        return;
    }
    int fw = (int)k.fb_w, fh = (int)k.fb_h;
    u32 *p = sh.phys.px;
    for (int ly = d.y; ly < d.y + d.h; ly++) {
        const u32 *row = src->px + (usize)ly * src->stride;
        switch (sh.rot) {
        case 1: { u32 *o = p + (fw - 1 - ly);            for (int lx = d.x; lx < d.x + d.w; lx++) o[(usize)lx * fw] = row[lx]; break; }
        case 2: { u32 *o = p + (usize)(fh - 1 - ly) * fw; for (int lx = d.x; lx < d.x + d.w; lx++) o[fw - 1 - lx] = row[lx]; break; }
        default: { u32 *o = p + ly;                        for (int lx = d.x; lx < d.x + d.w; lx++) o[(usize)(fh - 1 - lx) * fw] = row[lx]; break; }
        }
    }
    rect_t r;
    switch (sh.rot) {
    case 1:  r = (rect_t){ fw - (d.y + d.h), d.x, d.h, d.w }; break;
    case 2:  r = (rect_t){ fw - (d.x + d.w), fh - (d.y + d.h), d.w, d.h }; break;
    default: r = (rect_t){ d.y, fh - (d.x + d.w), d.h, d.w }; break;
    }
    hal_present(p, fw, r.x, r.y, r.w, r.h);
}

static void draw_cursor(canvas_t *c, int x, int y) {
    float s = ui.s;
    float pts[][2] = { {0, 0}, {0, 17}, {4.5f, 13}, {8, 20}, {10.5f, 19}, {7, 12}, {12.5f, 12} };
    for (int pass = 0; pass < 2; pass++) {
        u32 col = pass ? RGB(255, 255, 255) : RGBA(0, 0, 0, 200);
        float w = pass ? 1.6f * s : 3.6f * s;
        for (int i = 0; i < 7; i++) {
            float *a = pts[i], *b = pts[(i + 1) % 7];
            gfx_line(c, x + a[0] * s, y + a[1] * s, x + b[0] * s, y + b[1] * s, w, col);
        }
    }
}

/* ---- common chrome ---------------------------------------------------------- */
static void clock_text(char *buf, usize cap, const EFI_TIME *t) { fmt(buf, cap, "%02d:%02d", t->Hour, t->Minute); }

static const char *weekday(const EFI_TIME *t) {
    static const char *names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    int y = t->Year, m = t->Month, d = t->Day;
    static const int off[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    if (m < 1 || m > 12) return "";
    return names[(y + y / 4 - y / 100 + y / 400 + off[m - 1] + d) % 7];
}

static const char *month_name(int m) {
    static const char *n[] = { "January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December" };
    return (m >= 1 && m <= 12) ? n[m - 1] : "";
}

static void draw_status(canvas_t *c, const EFI_TIME *t) {
    int h = dp(32), pad = dp(20);
    char buf[48];
    const font_t *f = ui.small;
    int y = (h - f->line) / 2 + dp(2);
    gfx_text(c, ui.label->size <= f->size ? ui.label : font_pick(F_SEMIBOLD, dp(13)), pad, y, "QRT", ui.text);
    if (sh.view == VIEW_APP) {
        clock_text(buf, sizeof buf, t);
        gfx_text_center(c, font_pick(F_SEMIBOLD, dp(13)), (rect_t){ 0, 0, ui.W, h + dp(4) }, buf, ui.text);
    }
    const char *dev = k.is_venue ? "Venue 8 Pro" : (k.sys_product[0] ? k.sys_product : "UEFI PC");
    fmt(buf, sizeof buf, "%s  \xc2\xb7  %s", dev, QRT_ARCH);
    int w = text_width(f, buf);
    gfx_text(c, f, ui.W - pad - w, y, buf, ui.text2);
}

/* ---- geometry ----------------------------------------------------------------
 * A status bar across the top, the dock floating along one edge and the
 * content area (home, launcher, apps) filling the rest.  The dock is a
 * rounded panel inset from its edge; it can be dragged anywhere and snaps
 * to the nearest edge when released.  The on-screen keyboard, when shown,
 * takes the bottom of the content area. */
#define STATUS_H dp(32)
enum { DOCK_RIGHT, DOCK_LEFT, DOCK_BOTTOM, DOCK_TOP };
static int dock_thick(void) { return dp(76); }
static int dock_gap(void) { return dp(8); }
static int dock_band(void) { return dock_thick() + 2 * dock_gap(); }     /* space the dock takes from the content */
static int dock_vertical(int edge) { return edge == DOCK_RIGHT || edge == DOCK_LEFT; }
static int dock_items(int *out);

/* the panel's length along its edge: as long as its icons need, at most the edge */
static int dock_length(int edge) {
    int items[16], n = dock_items(items);
    int full = dock_vertical(edge) ? ui.H - STATUS_H - 2 * dock_gap() : ui.W - 2 * dock_gap();
    int want = dp(20) + n * dp(64) + dp(12) + dp(60);
    return MIN(want, full);
}

static rect_t dock_rect_at(int edge) {
    int t = dock_thick(), g = dock_gap(), len = dock_length(edge);
    int top = STATUS_H + g;
    /* side docks hang from the top so icons stay put as apps open; top and
     * bottom docks are centred */
    switch (edge) {
    case DOCK_LEFT:   return (rect_t){ g, top, t, len };
    case DOCK_BOTTOM: return (rect_t){ (ui.W - len) / 2, ui.H - g - t, len, t };
    case DOCK_TOP:    return (rect_t){ (ui.W - len) / 2, top, len, t };
    default:          return (rect_t){ ui.W - g - t, top, t, len };
    }
}

/* the edge nearest to a point: where a dragged dock lands */
static int nearest_edge(int x, int y) {
    int d[4] = { ui.W - x, x, ui.H - y, y - STATUS_H };
    int best = DOCK_RIGHT;
    for (int i = 1; i < 4; i++) if (d[i] < d[best]) best = i;
    return best;
}

/* While dragged, the panel takes the shape it would have on the nearest edge
 * and stays under the finger at the point where it was picked up.  After a
 * drop it glides to its edge (sh.anim_rect, advanced once per frame). */
static int dock_shape(void) { return sh.dock_drag == 2 ? nearest_edge(sh.drag_x, sh.drag_y) : sh.dock_edge; }
static rect_t dock_rect(void) {
    if (sh.animating) return sh.anim_rect;
    if (sh.dock_drag != 2) return dock_rect_at(sh.dock_edge);
    int shape = dock_shape();
    rect_t r = dock_rect_at(shape);
    int fx = sh.grab_fx, fy = sh.grab_fy;
    if (dock_vertical(shape) != sh.grab_vert) { int t = fx; fx = fy; fy = t; }   /* turned: keep the spot along its length */
    r.x = CLAMP(sh.drag_x - r.w * fx / 1000, 0, ui.W - r.w);
    r.y = CLAMP(sh.drag_y - r.h * fy / 1000, STATUS_H, ui.H - r.h);
    return r;
}
/* what a moving dock repaints: the panel and its drop shadow */
static rect_t dock_paint_rect(void) {
    rect_t d = dock_rect();
    int m = dp(26);
    return (rect_t){ d.x - m, d.y - m, d.w + 2 * m, d.h + 2 * m };
}

static rect_t content_rect(void) {
    rect_t c = { 0, STATUS_H, ui.W, ui.H - STATUS_H };
    int b = dock_band();
    switch (sh.dock_edge) {
    case DOCK_LEFT:   c.x += b; c.w -= b; break;
    case DOCK_BOTTOM: c.h -= b; break;
    case DOCK_TOP:    c.y += b; c.h -= b; break;
    default:          c.w -= b; break;
    }
    return c;
}
/* content minus the keyboard */
static rect_t work_rect(void) { rect_t c = content_rect(); c.h -= osk_height(); return c; }

/* ---- dock ------------------------------------------------------------------- */
static const app_t *const pinned[] = { &app_files, &app_terminal, &app_browser, &app_wifi, &app_sketch, &app_settings };

static int app_index(const app_t *a) { for (int i = 0; i < N_APPS; i++) if (apps[i] == a) return i; return -1; }

/* pinned apps first, then apps that were opened and are not pinned */
static int dock_items(int *out) {
    int n = 0;
    for (usize i = 0; i < ARRAY_LEN(pinned); i++) { int ix = app_index(pinned[i]); if (ix >= 0) out[n++] = ix; }
    for (int i = 0; i < N_APPS; i++) {
        if (!(sh.running & (1u << i))) continue;
        int dup = 0;
        for (int j = 0; j < n; j++) dup |= out[j] == i;
        if (!dup) out[n++] = i;
    }
    return n;
}

/* cells shrink when many apps are open, so every one keeps a place */
static int dock_cell(void) {
    int items[16], n = dock_items(items);
    rect_t d = dock_rect();
    int len = dock_vertical(dock_shape()) ? d.h : d.w;
    int avail = len - dp(20) - dp(60) - dp(12);                  /* minus the launcher button */
    return CLAMP(avail / MAX(1, n) - dp(4), dp(40), dp(60));
}
static rect_t dock_slot(int i) {
    rect_t d = dock_rect();
    int c = dock_cell(), along = dp(10) + i * (c + dp(4));
    if (dock_vertical(dock_shape())) return (rect_t){ d.x + (d.w - c) / 2, d.y + along, c, c };
    return (rect_t){ d.x + along, d.y + (d.h - c) / 2, c, c };
}
static rect_t dock_launcher_rect(void) {
    rect_t d = dock_rect();
    int c = dp(60);
    if (dock_vertical(dock_shape())) return (rect_t){ d.x + (d.w - c) / 2, d.y + d.h - dp(10) - c, c, c };
    return (rect_t){ d.x + d.w - dp(10) - c, d.y + (d.h - c) / 2, c, c };
}

void shell_set_dock_edge(int edge) {
    sh.dock_edge = edge & 3;
    hal_setting_set(u"QrtDockEdge", (u32)sh.dock_edge);
    sh.dirty = 1;
}
int shell_dock_edge(void) { return sh.dock_edge; }

static void app_icon(canvas_t *c, const app_t *a, float cx, float cy, float r) {
    gfx_circle(c, cx, cy, r, a->color);
    a->icon(c, cx, cy, r * 0.62f, RGB(255, 255, 255));
}

static void draw_dock(canvas_t *c) {
    rect_t d = dock_rect();
    int edge = dock_shape(), vert = dock_vertical(edge), rad = dp(22);
    int lifted = sh.dock_drag == 2 || sh.animating;
    if (lifted) gfx_shadow(c, d, rad, dp(18), RGBA(0, 0, 0, 120));
    gfx_rrect(c, d, rad, RGBA(8, 6, 18, lifted ? 215 : 185));
    gfx_rrect_outline(c, d, rad, 1, lifted ? ALPHA(ui.accent, 160) : ui.stroke);
    int items[16], n = dock_items(items);
    rect_t lr = dock_launcher_rect();
    for (int i = 0; i < n; i++) {
        rect_t r = dock_slot(i);
        if (vert ? r.y + r.h > lr.y - dp(4) : r.x + r.w > lr.x - dp(4)) break;     /* no room left */
        const app_t *a = apps[items[i]];
        int active = sh.view == VIEW_APP && sh.app == a && !sh.launcher_open;
        if (active || (sh.dock_pressed == i && sh.dock_drag != 2)) gfx_rrect(c, r, dp(14), RGBA(255, 255, 255, active ? 42 : 28));
        app_icon(c, a, r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w * 0.35f);
        if (sh.running & (1u << items[i])) {
            /* the running dot sits on the side facing the screen's middle */
            float dx, dy;
            switch (edge) {
            case DOCK_LEFT:   dx = d.x + d.w - dp(7); dy = r.y + r.h / 2.0f; break;
            case DOCK_BOTTOM: dx = r.x + r.w / 2.0f; dy = d.y + dp(6); break;
            case DOCK_TOP:    dx = r.x + r.w / 2.0f; dy = d.y + d.h - dp(6); break;
            default:          dx = d.x + dp(7); dy = r.y + r.h / 2.0f; break;
            }
            gfx_circle(c, dx, dy, dp(2.5f), active ? ui.accent : ui.text);
        }
    }
    /* "show applications": a 3x3 grid of dots */
    if (sh.launcher_open || (sh.dock_pressed == 99 && sh.dock_drag != 2)) gfx_rrect(c, lr, dp(14), RGBA(255, 255, 255, sh.launcher_open ? 42 : 28));
    float cx = lr.x + lr.w / 2.0f, cy = lr.y + lr.h / 2.0f, g = dp(8);
    for (int yy = -1; yy <= 1; yy++)
        for (int xx = -1; xx <= 1; xx++) gfx_circle(c, cx + xx * g, cy + yy * g, dp(2.6f), ui.text);
}

/* ---- home ------------------------------------------------------------------- */
static void draw_home(canvas_t *c, const EFI_TIME *t) {
    rect_t area = content_rect();
    int pad = area.x + dp(28);
    char buf[64];
    int y = area.y + (ui.landscape ? dp(28) : dp(48));
    int colw = area.w - 2 * dp(28);

    clock_text(buf, sizeof buf, t);
    gfx_text(c, ui.huge, pad - dp(4), y, buf, ui.text);
    y += ui.huge->line;
    fmt(buf, sizeof buf, "%s, %d %s", weekday(t), t->Day, month_name(t->Month));
    gfx_text(c, ui.title, pad, y, buf, ui.text);
    y += ui.title->line + dp(6);
    const char *greet = t->Hour < 5 ? "Up late" : t->Hour < 12 ? "Good morning" : t->Hour < 18 ? "Good afternoon" : "Good evening";
    fmt(buf, sizeof buf, k.native ? "%s. Running on the Tessera kernel." : "%s. Everything runs on your firmware.", greet);
    gfx_text_fit(c, ui.body, pad, y, colw, buf, ui.text2);
    y += ui.body->line + dp(24);

    char ram[24], line[96];
    fmt_bytes(ram, sizeof ram, k.ram_bytes);
    fmt(line, sizeof line, "%s  \xc2\xb7  %s RAM", k.cpu, ram);
    gfx_text_fit(c, ui.small, pad, y, colw, line, ui.text3);
    const char *net = shell_net_status();
    if (net) gfx_text_fit(c, ui.small, pad, y + ui.small->line, colw, net, ui.text3);

    const char *hint = "The dots in the dock show every app. Drag the dock to move it to another edge.";
    gfx_text_fit(c, ui.small, pad, area.y + area.h - dp(24) - ui.small->line, colw, hint, ui.text3);
}

/* ---- launcher: every app, plus search over apps and actions ------------------ */
typedef struct { const char *title, *sub; int app; int action; } suggestion_t;
enum { ACT_NONE, ACT_SHUTDOWN, ACT_REBOOT, ACT_FIRMWARE, ACT_ROTATE, ACT_ACCENT,
       ACT_TOUCH_SWAP, ACT_TOUCH_FLIPX, ACT_TOUCH_FLIPY, ACT_TOUCH_RESET, ACT_BENCH, ACT_SMP, ACT_KEYBOARD };

static const suggestion_t actions[] = {
    { "Rotate screen", "Turn the canvas 90\xc2\xb0", -1, ACT_ROTATE },
    { "Next accent colour", "Cycle the theme", -1, ACT_ACCENT },
    { "Restart", "Cold reset through UEFI", -1, ACT_REBOOT },
    { "Shut down", "Power off through UEFI", -1, ACT_SHUTDOWN },
    { "Firmware setup", "Reboot into the BIOS/UEFI menu", -1, ACT_FIRMWARE },
    { "Graphics benchmark", "Time full and partial redraws (bench)", -1, ACT_BENCH },
    { "Multicore rendering on/off", "Draw with the other CPU cores", -1, ACT_SMP },
    { "On-screen keyboard", "Show or hide the keyboard", -1, ACT_KEYBOARD },
    { "Touch: swap axes", "Fix a touchscreen mounted sideways", -1, ACT_TOUCH_SWAP },
    { "Touch: flip X", "Mirror touch left-right", -1, ACT_TOUCH_FLIPX },
    { "Touch: flip Y", "Mirror touch top-bottom", -1, ACT_TOUCH_FLIPY },
    { "Touch: reset", "Use the firmware's mapping as-is", -1, ACT_TOUCH_RESET },
};

static int match_apps(int *out) {
    int n = 0;
    for (int i = 0; i < N_APPS; i++)
        if (!sh.query[0] || str_icontains(apps[i]->name, sh.query) || str_icontains(apps[i]->blurb, sh.query)) out[n++] = i;
    return n;
}
static int match_actions(int *out) {
    int n = 0;
    if (!sh.query[0]) return 0;
    for (int i = 0; i < (int)ARRAY_LEN(actions); i++)
        if (str_icontains(actions[i].title, sh.query) || str_icontains(actions[i].sub, sh.query)) out[n++] = i;
    return n;
}

static rect_t search_rect(void) {
    rect_t a = work_rect();
    int w = MIN(a.w - dp(48), dp(520));
    return (rect_t){ a.x + (a.w - w) / 2, a.y + dp(20), w, dp(48) };
}
static int grid_cols(void) { rect_t a = work_rect(); return MAX(1, MIN(6, (a.w - dp(32)) / dp(116))); }
static rect_t grid_cell(int i) {
    rect_t a = work_rect(), s = search_rect();
    int cols = grid_cols(), cw = dp(116), ch = dp(112);
    int x0 = a.x + (a.w - cols * cw) / 2;
    return (rect_t){ x0 + (i % cols) * cw, s.y + s.h + dp(40) + (i / cols) * ch, cw, ch };
}
static rect_t action_row(int napps, int i) {
    rect_t s = search_rect();
    int rows = (napps + grid_cols() - 1) / grid_cols();
    int y0 = napps ? grid_cell((rows - 1) * grid_cols()).y + dp(112) + dp(28) : s.y + s.h + dp(40);
    return (rect_t){ s.x, y0 + i * dp(56), s.w, dp(52) };
}

static void draw_launcher(canvas_t *c) {
    rect_t a = content_rect();
    gfx_fill(c, a, RGBA(6, 4, 14, 215));
    rect_t f = search_rect();
    gfx_rrect(c, f, f.h / 2, RGBA(0xf5, 0xf3, 0xfa, 235));
    float cx = f.x + dp(26), cy = f.y + f.h / 2.0f;
    gfx_ring(c, cx - dp(2), cy - dp(2), dp(8), dp(2.4f), ui.accent);
    gfx_line(c, cx + dp(4), cy + dp(4), cx + dp(9), cy + dp(9), dp(2.6f), ui.accent);
    int tx = f.x + dp(48), ty = f.y + (f.h - ui.body->line) / 2;
    int end = sh.query[0] ? gfx_text(c, ui.body, tx, ty, sh.query, RGB(0x20, 0x1a, 0x30))
                          : (gfx_text(c, ui.body, tx, ty, "Search apps and actions", RGBA(0x20, 0x1a, 0x30, 120)), tx);
    gfx_fill(c, (rect_t){ end + 1, ty + dp(2), MAX(1, dp(2)), ui.body->line - dp(4) }, ui.accent);

    int ia[32], na = match_apps(ia), ix[32], nx = match_actions(ix);
    rect_t w = work_rect();
    gfx_clip(c, w);
    ui_section(c, grid_cell(0).x + dp(12), f.y + f.h + dp(14), sh.query[0] ? "APPS" : "ALL APPS");
    for (int i = 0; i < na; i++) {
        rect_t r = grid_cell(i);
        if (sh.launch_pressed == i) gfx_rrect(c, (rect_t){ r.x + dp(6), r.y, r.w - dp(12), r.h - dp(6) }, dp(18), RGBA(255, 255, 255, 30));
        const app_t *ap = apps[ia[i]];
        app_icon(c, ap, r.x + r.w / 2.0f, r.y + dp(40), dp(29));
        if (sh.query[0] && i == 0) gfx_ring(c, r.x + r.w / 2.0f, r.y + dp(40), dp(33), dp(2), ui.accent);
        gfx_text_center(c, ui.label, (rect_t){ r.x + dp(4), r.y + dp(76), r.w - dp(8), ui.label->line }, ap->name, ui.text);
    }
    if (nx) ui_section(c, action_row(na, 0).x + dp(12), action_row(na, 0).y - ui.small->line - dp(6), "ACTIONS");
    for (int i = 0; i < nx; i++) {
        rect_t r = action_row(na, i);
        gfx_rrect(c, r, dp(14), sh.launch_pressed == 100 + i || (!na && i == 0) ? RGBA(255, 255, 255, 40) : RGBA(255, 255, 255, 16));
        gfx_circle(c, r.x + dp(24), r.y + r.h / 2.0f, dp(14), RGB(0x55, 0x50, 0x66));
        gfx_ring(c, r.x + dp(24), r.y + r.h / 2.0f, dp(6), dp(2), RGB(255, 255, 255));
        gfx_text(c, ui.label, r.x + dp(48), r.y + dp(6), actions[ix[i]].title, ui.text);
        gfx_text_fit(c, ui.small, r.x + dp(48), r.y + dp(6) + ui.label->line, r.w - dp(56), actions[ix[i]].sub, ui.text2);
    }
    gfx_unclip(c);
}

/* ---- power menu (hardware power button) ---------------------------------------- */
static const char *power_items[] = { "Sleep", "Restart", "Shut down", "Firmware setup", "Cancel" };
#define N_POWER 5
static rect_t power_panel(void) {
    int w = MIN(ui.W - dp(40), dp(360)), h = dp(76) + N_POWER * dp(56);
    return (rect_t){ (ui.W - w) / 2, (ui.H - h) / 2, w, h };
}
static rect_t power_row(int i) { rect_t p = power_panel(); return (rect_t){ p.x + dp(14), p.y + dp(64) + i * dp(56), p.w - dp(28), dp(50) }; }
static void draw_power(canvas_t *c) {
    gfx_fill(c, full_rect(), RGBA(0, 0, 0, 150));
    rect_t p = power_panel();
    ui_card(c, p, dp(24), 1);
    gfx_text(c, ui.title, p.x + dp(20), p.y + dp(20), "Power", ui.text);
    for (int i = 0; i < N_POWER; i++)
        ui_button(c, power_row(i), power_items[i], i == 2 ? RGB(0xe5, 0x48, 0x4d) : RGBA(255, 255, 255, 30), ui.text);
}

/* ---- app chrome ----------------------------------------------------------------- */
static rect_t app_area(void) {
    rect_t w = work_rect();
    int top = content_rect().y + dp(64);
    return (rect_t){ w.x, top, w.w, w.y + w.h - top - dp(10) };
}
rect_t shell_app_area(void) { return app_area(); }
static rect_t back_rect(void) { rect_t c = content_rect(); return (rect_t){ c.x + dp(14), c.y + dp(8), dp(48), dp(48) }; }

static void draw_app(canvas_t *c) {
    rect_t b = back_rect();
    gfx_circle(c, b.x + b.w / 2.0f, b.y + b.h / 2.0f, b.w / 2.0f, RGBA(255, 255, 255, 26));
    float cx = b.x + b.w / 2.0f, cy = b.y + b.h / 2.0f, a = dp(8);
    gfx_line(c, cx - a, cy, cx + a, cy, dp(2.4f), ui.text);
    gfx_line(c, cx - a, cy, cx - a * 0.2f, cy - a * 0.8f, dp(2.4f), ui.text);
    gfx_line(c, cx - a, cy, cx - a * 0.2f, cy + a * 0.8f, dp(2.4f), ui.text);
    gfx_circle(c, b.x + b.w + dp(22), cy, dp(6), sh.app->color);
    gfx_text(c, ui.title, b.x + b.w + dp(36), (int)cy - ui.title->line / 2, sh.app->name, ui.text);

    rect_t area = app_area();
    gfx_clip(c, area);
    sh.app->draw(c, area);
    gfx_unclip(c);
}

/* ---- compose ---------------------------------------------------------------- */
/*
 * Redraw only rectangle d of the scene.  Everything is drawn as before but
 * with d as the canvas limit, so primitives outside it are rejected by their
 * clip test and the cost scales with the damaged area, not the screen.
 */
static void draw_lock(canvas_t *c, const EFI_TIME *t);
static void draw_osd(canvas_t *c);
static EFI_TIME frame_time;           /* wall clock sampled once per frame on the boot core */
void shell_time(EFI_TIME *t) { *t = frame_time; }

/* Draw rectangle d into the scene.  Runs on any core: it touches only pixels
 * inside d and uses a private copy of the canvas's clip state. */
static void compose_rect(rect_t d) {
    canvas_t c = sh.scene;
    gfx_limit(&c, d);
    d = c.limit;
    const canvas_t *wall = !sh.locked && (sh.view == VIEW_APP || sh.launcher_open) ? &sh.wall_dim : &sh.wall;
    for (int y = d.y; y < d.y + d.h; y++)
        memcpy(c.px + (usize)y * c.stride + d.x, wall->px + (usize)y * wall->stride + d.x, (usize)d.w * 4);
    draw_status(&c, &frame_time);
    if (sh.locked) draw_lock(&c, &frame_time);
    else {
        if (sh.view == VIEW_HOME) draw_home(&c, &frame_time);
        else draw_app(&c);
        if (sh.launcher_open) draw_launcher(&c);
        osk_draw(&c, content_rect());
        draw_dock(&c);
    }
    if (sh.power_open) draw_power(&c);
    if (sh.osd_shown) draw_osd(&c);
}

static rect_t compose_area;
static void compose_job(void *arg, int i, int n) { compose_rect(band(compose_area, i, n)); }

static int force_single_core;

static void compose(rect_t d) {
    k_walltime(&frame_time);
    if (d.w >= ui.W && d.h >= ui.H) sh.last_minute = frame_time.Minute;
    d = rect_intersect(d, full_rect());
    /* waking the other cores costs tens of microseconds: only for big areas */
    int n = force_single_core ? 0 : smp_workers();
    if (n && (u64)d.w * d.h >= 160000 && d.h >= 16 * n) {
        /* 4 strips per core: strips are claimed dynamically, so cores that
         * land on cheap areas (plain wallpaper) simply take more of them */
        compose_area = d;
        smp_run(compose_job, NULL, MIN(4 * n, 64));
    } else {
        compose_rect(d);
    }
}

/* ---- navigation ---------------------------------------------------------------- */
static void open_app(int i) {
    sh.app = apps[i];
    sh.view = VIEW_APP;
    sh.launcher_open = 0;
    sh.query[0] = 0;
    sh.running |= 1u << i;
    osk_hide();
    if (sh.app->open) sh.app->open();
    sh.dirty = 1;
}

void shell_go_home(void) {
    sh.view = VIEW_HOME;
    sh.app = NULL;
    sh.launcher_open = 0;
    osk_hide();
    sh.dirty = 1;
}

void shell_keyboard(int show) {
    if (show && !osk_visible()) { osk_show(); sh.dirty = 1; }
    else if (!show && osk_visible()) { osk_hide(); sh.dirty = 1; }
}

static void open_launcher(int on) {
    sh.launcher_open = on;
    sh.query[0] = 0;
    sh.launch_pressed = -1;
    if (!on) osk_hide();
    sh.dirty = 1;
}

static void run_action(int act) {
    switch (act) {
    case ACT_SHUTDOWN: hal_shutdown(); break;
    case ACT_REBOOT: hal_reboot(); break;
    case ACT_FIRMWARE: hal_reboot_to_firmware(); break;
    case ACT_ROTATE: shell_set_rotation(sh.rot + 1); break;
    case ACT_ACCENT: shell_set_accent((sh.accent_idx + 1) % N_ACCENTS); break;
    case ACT_TOUCH_SWAP:  hal_set_touch_map(k.touch_map ^ TOUCH_SWAP_XY); break;
    case ACT_TOUCH_FLIPX: hal_set_touch_map(k.touch_map ^ TOUCH_FLIP_X); break;
    case ACT_TOUCH_FLIPY: hal_set_touch_map(k.touch_map ^ TOUCH_FLIP_Y); break;
    case ACT_TOUCH_RESET: hal_set_touch_map(0); break;
    case ACT_BENCH: sh.bench_pending = 1; break;
    case ACT_SMP: smp_set_enabled(!smp_enabled()); break;
    case ACT_KEYBOARD: sh.keyboard_pending = 1; break;
    }
}

/* Enter in the launcher: the first app, else the first action. */
static void launcher_activate_first(void) {
    int ia[32], na = match_apps(ia), ix[32], nx = match_actions(ix);
    if (!strcmp(sh.query, "bench")) { open_launcher(0); run_action(ACT_BENCH); return; }
    if (na) { open_app(ia[0]); return; }
    if (nx) { int act = actions[ix[0]].action; open_launcher(0); run_action(act); }
}

static void launcher_key(const event_t *e) {
    usize n = strlen(sh.query);
    if (e->scan == SCAN_ESC) open_launcher(0);
    else if (e->ch == '\r' || e->ch == '\n') launcher_activate_first();
    else if (e->ch == 8) { if (n) sh.query[n - 1] = 0; }
    else if (e->ch >= 32 && e->ch < 127 && n + 1 < sizeof sh.query) { sh.query[n] = (char)e->ch; sh.query[n + 1] = 0; }
    sh.dirty = 1;
}

static void launcher_pointer(const event_t *e) {
    int ia[32], na = match_apps(ia), ix[32], nx = match_actions(ix);
    int hit = -1;
    rect_t w = work_rect();
    for (int i = 0; i < na; i++) if (in_rect(grid_cell(i), e->x, e->y) && in_rect(w, e->x, e->y)) hit = i;
    for (int i = 0; i < nx; i++) if (in_rect(action_row(na, i), e->x, e->y) && in_rect(w, e->x, e->y)) hit = 100 + i;
    if (e->type == EV_DOWN) { sh.launch_pressed = hit; sh.dirty = 1; }
    if (tap_track(&sh.tap, e, dp(12))) {
        if (in_rect(search_rect(), e->x, e->y)) shell_keyboard(1);
        else if (hit >= 0 && hit == sh.launch_pressed) {
            if (hit < 100) open_app(ia[hit]);
            else { int act = actions[ix[hit - 100]].action; open_launcher(0); run_action(act); }
        } else if (hit < 0) open_launcher(0);
    }
    if (e->type == EV_UP) { sh.launch_pressed = -1; sh.dirty = 1; }
}

/* A touch on the dock is a tap (open an app, or the launcher) or, once it
 * moves past a threshold, a drag: the dock follows the finger and, on
 * release, glides to the nearest edge.  Only the panel's own area is
 * repainted while it moves, so dragging costs a small fraction of a frame. */
static void dock_pointer(const event_t *e) {
    int items[16], n = dock_items(items), hit = -1;
    for (int i = 0; i < n; i++) if (in_rect(dock_slot(i), e->x, e->y)) hit = i;
    if (in_rect(dock_launcher_rect(), e->x, e->y)) hit = 99;
    switch (e->type) {
    case EV_DOWN: {
        rect_t d = dock_rect();
        sh.animating = 0;
        sh.dock_pressed = hit;
        sh.dock_drag = 1;
        sh.drag_x0 = e->x; sh.drag_y0 = e->y;
        sh.grab_fx = CLAMP((e->x - d.x) * 1000 / MAX(1, d.w), 0, 1000);
        sh.grab_fy = CLAMP((e->y - d.y) * 1000 / MAX(1, d.h), 0, 1000);
        sh.grab_vert = dock_vertical(sh.dock_edge);
        shell_damage(dock_rect());
        break;
    }
    case EV_MOVE:
        if (sh.dock_drag == 1 && (ABS_I(e->x - sh.drag_x0) > dp(14) || ABS_I(e->y - sh.drag_y0) > dp(14))) {
            sh.dock_drag = 2;
            sh.dock_pressed = -1;
            shell_damage(dock_paint_rect());
        }
        if (sh.dock_drag == 2) {
            shell_damage(dock_paint_rect());                 /* where it was */
            sh.drag_x = e->x; sh.drag_y = e->y;
            shell_damage(dock_paint_rect());                 /* where it is */
        }
        break;
    default: break;
    }
    int tap = tap_track(&sh.tap, e, dp(12));
    if (e->type != EV_UP) return;
    if (sh.dock_drag == 2) {
        int edge = nearest_edge(e->x, e->y);
        sh.anim_from = sh.anim_rect = dock_rect();
        sh.dock_drag = 0;
        sh.dock_pressed = -1;
        if (edge != sh.dock_edge) { shell_set_dock_edge(edge); osk_hide(); }   /* content moves: one full repaint */
        sh.anim_to = dock_rect_at(sh.dock_edge);
        sh.animating = 1;
        sh.anim_t0 = k_now_ms();
        return;
    }
    sh.dock_drag = 0;
    if (tap) {
        if (hit == 99 && sh.dock_pressed == 99) open_launcher(!sh.launcher_open);
        else if (hit >= 0 && hit == sh.dock_pressed) {
            const app_t *a = apps[items[hit]];
            /* like Ubuntu: tapping the app in front minimises it */
            if (sh.view == VIEW_APP && sh.app == a && !sh.launcher_open) shell_go_home();
            else open_app(items[hit]);
        }
    }
    sh.dock_pressed = -1;
    shell_damage(dock_rect());
}

/* one step of the snap animation (main loop, once per frame) */
static void dock_animate(u64 now) {
    if (!sh.animating) return;
    shell_damage(dock_paint_rect());
    float t = (float)(now - sh.anim_t0) / 150.0f;
    if (t >= 1) { sh.animating = 0; shell_damage(dock_rect()); return; }
    float e = 1 - (1 - t) * (1 - t) * (1 - t);             /* ease out */
    rect_t a = sh.anim_from, b = sh.anim_to;
    sh.anim_rect = (rect_t){ a.x + (int)((b.x - a.x) * e), a.y + (int)((b.y - a.y) * e),
                             a.w + (int)((b.w - a.w) * e), a.h + (int)((b.h - a.h) * e) };
    shell_damage(dock_paint_rect());
}

static void go_to_sleep(void);
static void power_pointer(const event_t *e) {
    if (!tap_track(&sh.tap, e, dp(12))) return;
    int hit = -1;
    for (int i = 0; i < N_POWER; i++) if (in_rect(power_row(i), e->x, e->y)) hit = i;
    sh.power_open = 0;
    sh.dirty = 1;
    if (hit == 0) go_to_sleep();
    else if (hit == 1) hal_reboot();
    else if (hit == 2) hal_shutdown();
    else if (hit == 3) hal_reboot_to_firmware();
}

/* ---- volume (mock audio: there is no sound driver yet) ---------------------------- */
static rect_t osd_rect(void) {
    int w = MIN(ui.W - dp(40), dp(320)), h = dp(60);
    return (rect_t){ (ui.W - w) / 2, STATUS_H + dp(14), w, h };
}
static rect_t osd_paint_rect(void) { rect_t r = osd_rect(); int m = dp(20); return (rect_t){ r.x - m, r.y - m, r.w + 2 * m, r.h + 2 * m }; }
static void speaker_icon(canvas_t *c, float x, float cy, float s, int muted, int level, u32 fg) {
    /* body and cone */
    gfx_rrect(c, (rect_t){ (int)x, (int)(cy - s * 0.25f), (int)(s * 0.3f), (int)(s * 0.5f) }, dp(2), fg);
    for (int i = 0; i <= 6; i++) {
        float t = i / 6.0f;
        gfx_line(c, x + s * 0.3f, cy - s * 0.25f + t * s * 0.5f, x + s * 0.62f, cy - s * 0.5f + t * s, dp(2), fg);
    }
    if (muted) {
        gfx_line(c, x + s * 0.8f, cy - s * 0.2f, x + s * 1.2f, cy + s * 0.2f, dp(2.4f), fg);
        gfx_line(c, x + s * 0.8f, cy + s * 0.2f, x + s * 1.2f, cy - s * 0.2f, dp(2.4f), fg);
        return;
    }
    /* sound waves: one to three arcs */
    int waves = level > 66 ? 3 : level > 33 ? 2 : 1;
    for (int w = 0; w < waves; w++) {
        float r = s * (0.3f + 0.22f * w), cx = x + s * 0.55f;
        float px = cx + r * fcos(-0.9f), py = cy + r * fsin(-0.9f);
        for (int k2 = 1; k2 <= 6; k2++) {
            float a = -0.9f + 1.8f * k2 / 6;
            float qx = cx + r * fcos(a), qy = cy + r * fsin(a);
            gfx_line(c, px, py, qx, qy, dp(2.2f), fg);
            px = qx; py = qy;
        }
    }
}
static void draw_osd(canvas_t *c) {
    rect_t r = osd_rect();
    gfx_shadow(c, r, r.h / 2, dp(12), RGBA(0, 0, 0, 90));
    gfx_rrect(c, r, r.h / 2, RGBA(20, 16, 34, 235));
    gfx_rrect_outline(c, r, r.h / 2, 1, ui.stroke);
    float cy = r.y + r.h / 2.0f;
    speaker_icon(c, r.x + dp(20), cy, dp(26), sh.volume == 0, sh.volume, ui.text);
    char pct[16];
    fmt(pct, sizeof pct, sh.volume ? "%d%%" : "Muted", sh.volume);
    int tw = text_width(ui.label, pct);
    gfx_text(c, ui.label, r.x + r.w - dp(20) - tw, (int)cy - ui.label->line / 2, pct, ui.text);
    int bx = r.x + dp(64), bw = r.w - dp(64) - dp(34) - text_width(ui.label, "100%");
    rect_t track = { bx, (int)cy - dp(3), bw, dp(6) };
    gfx_rrect(c, track, dp(3), RGBA(255, 255, 255, 40));
    if (sh.volume) gfx_rrect(c, (rect_t){ bx, track.y, MAX(dp(6), bw * sh.volume / 100), track.h }, dp(3), ui.accent);
}
int shell_volume(void) { return sh.volume; }
void shell_set_volume(int v) {
    sh.volume = CLAMP(v, 0, 100);
    sh.osd_until = k_now_ms() + 1500;
    sh.osd_shown = 1;
    shell_damage(osd_paint_rect());
    klog("volume: %d%% (mock audio: no sound driver yet)", sh.volume);
}

/* ---- lock screen and sleep --------------------------------------------------------- */
int shell_sleep_after(void) { return sh.sleep_after; }
void shell_set_sleep_after(int seconds) {
    sh.sleep_after = MAX(0, seconds);
    hal_setting_set(u"QrtSleepAfter", (u32)sh.sleep_after);
}
static void lock_screen(void) {
    if (sh.locked) return;
    sh.locked = 1;
    sh.launcher_open = 0;
    sh.lock_dragging = sh.lock_armed = 0;
    osk_hide();
    sh.dirty = 1;
    klog("shell: locked");
}
static void go_to_sleep(void) {
    lock_screen();
    if (sh.asleep) return;
    sh.asleep = 1;
    sh.power_open = 0;
    sh.cursor_on = 0;
    backlight_power(0);
    /* no backlight control (QEMU, firmware mode): at least a black panel */
    memset(sh.scene.px, 0, (usize)sh.scene.stride * ui.H * 4);
    present(&sh.scene, full_rect());
    klog("shell: asleep%s", backlight_available() ? " (backlight off)" : " (screen blanked)");
}
static void wake_up(void) {
    if (!sh.asleep) return;
    sh.asleep = 0;
    backlight_power(1);
    sh.last_input_ms = k_now_ms();
    sh.dirty = 1;
    klog("shell: awake");
}
static void unlock(void) {
    sh.locked = 0;
    sh.lock_dragging = sh.lock_armed = 0;
    sh.dirty = 1;
}
static rect_t lock_hint_rect(void) { return (rect_t){ 0, ui.H - dp(120), ui.W, dp(100) }; }
static void draw_lock(canvas_t *c, const EFI_TIME *t) {
    char buf[64];
    clock_text(buf, sizeof buf, t);
    int y = ui.H * 30 / 100;
    int w = text_width(ui.huge, buf);
    gfx_text(c, ui.huge, (ui.W - w) / 2, y, buf, ui.text);
    y += ui.huge->line;
    fmt(buf, sizeof buf, "%s, %d %s", weekday(t), t->Day, month_name(t->Month));
    gfx_text_center(c, ui.title, (rect_t){ 0, y, ui.W, ui.title->line }, buf, ui.text);
    y += ui.title->line + dp(10);
    const char *net = shell_net_status();
    if (net) gfx_text_center(c, ui.small, (rect_t){ 0, y, ui.W, ui.small->line }, net, ui.text2);
    rect_t h = lock_hint_rect();
    float cx = ui.W / 2.0f, cy = h.y + dp(28);
    u32 col = sh.lock_armed ? ui.accent : ui.text2;
    gfx_line(c, cx - dp(12), cy + dp(6), cx, cy - dp(6), dp(3), col);
    gfx_line(c, cx, cy - dp(6), cx + dp(12), cy + dp(6), dp(3), col);
    gfx_text_center(c, ui.label, (rect_t){ 0, h.y + dp(46), ui.W, ui.label->line },
                    sh.lock_armed ? "Release to unlock" : "Swipe up to unlock", col);
}
static void lock_pointer(const event_t *e) {
    if (e->type == EV_DOWN) { sh.lock_dragging = 1; sh.lock_y0 = e->y; sh.lock_armed = 0; }
    else if (e->type == EV_MOVE && sh.lock_dragging) {
        int armed = sh.lock_y0 - e->y > dp(110);
        if (armed != sh.lock_armed) { sh.lock_armed = armed; shell_damage(lock_hint_rect()); }
    } else if (e->type == EV_UP) {
        int go = sh.lock_dragging && sh.lock_armed;
        sh.lock_dragging = 0;
        if (go) unlock();
        else if (sh.lock_armed) { sh.lock_armed = 0; shell_damage(lock_hint_rect()); }
    }
}

/* Hardware buttons.  Power: lock (on the lock screen: sleep); held for a
 * second: the power menu.  Volume: mock audio volume with an on-screen
 * indicator.  Windows: the launcher (the dock's app list). */
static int hardware_key(const event_t *e) {
    switch (e->scan) {
    case SCAN_POWER:
        sh.power_open = 0;
        if (sh.locked) go_to_sleep(); else lock_screen();
        sh.dirty = 1;
        return 1;
    case SCAN_POWER_LONG: sh.power_open = 1; sh.dirty = 1; return 1;
    case SCAN_VOLUP: shell_set_volume(sh.volume + 5); return 1;
    case SCAN_VOLDN: shell_set_volume(sh.volume - 5); return 1;
    case SCAN_HOMEBTN:
        if (sh.locked) return 1;
        sh.power_open = 0;
        open_launcher(!sh.launcher_open);
        return 1;
    }
    return 0;
}

static void key_event(const event_t *e) {
    if (hardware_key(e)) return;
    if (sh.power_open) { if (e->scan == SCAN_ESC) { sh.power_open = 0; sh.dirty = 1; } return; }
    if (sh.locked) { if (e->ch == '\r' || e->ch == ' ') unlock(); return; }   /* a keyboard unlocks with Enter */
    if (sh.launcher_open) { launcher_key(e); return; }
    if (sh.view == VIEW_HOME) {
        /* typing on the home screen searches */
        if (e->ch >= 32 && e->ch < 127) { open_launcher(1); launcher_key(e); }
        return;
    }
    if (e->scan == SCAN_ESC) { shell_go_home(); return; }
    sh.app_damaged = 0;
    if (sh.app->event && sh.app->event(e, app_area()) && !sh.app_damaged) shell_damage(app_area());
}

enum { OWN_NONE, OWN_OSK, OWN_DOCK, OWN_MAIN, OWN_POWER, OWN_LOCK };

static void dispatch(event_t e) {
    sh.last_input_ms = sh.now_ms;
    if (sh.asleep) {
        /* asleep: power or the Windows button (or a keyboard) wakes it; touch and volume do not,
         * unless there are no hardware buttons (firmware mode, other machines) */
        if (e.type == EV_KEY && e.scan != SCAN_VOLUP && e.scan != SCAN_VOLDN && e.scan != SCAN_POWER_LONG) wake_up();
        else if (e.type == EV_DOWN && !buttons_active()) { wake_up(); sh.owner = OWN_NONE; }
        return;
    }
    if (e.type == EV_KEY) { key_event(&e); return; }
    int lx, ly;
    to_logical(e.x, e.y, &lx, &ly);
    e.x = lx; e.y = ly;
    if (e.from_mouse) { sh.cursor_x = lx; sh.cursor_y = ly; sh.cursor_on = 1; sh.cursor_dirty = 1; }
    else if (sh.cursor_on) { sh.cursor_on = 0; sh.cursor_dirty = 1; }

    /* a touch belongs to whatever it started on until it lifts */
    if (e.type == EV_DOWN || e.type == EV_SCROLL) {
        if (sh.power_open) sh.owner = OWN_POWER;
        else if (sh.locked) sh.owner = OWN_LOCK;
        else if (osk_visible() && in_rect(osk_rect(content_rect()), e.x, e.y)) sh.owner = OWN_OSK;
        else if (in_rect(dock_rect(), e.x, e.y)) sh.owner = OWN_DOCK;
        else sh.owner = OWN_MAIN;
    }
    int owner = sh.owner;
    if (e.type == EV_UP || e.type == EV_SCROLL) sh.owner = OWN_NONE;

    switch (owner) {
    case OWN_POWER: power_pointer(&e); return;
    case OWN_LOCK: lock_pointer(&e); return;
    case OWN_OSK: {
        event_t keys[4];
        int n = osk_pointer(&e, content_rect(), keys, 4);
        shell_damage(osk_rect(content_rect()));
        if (!osk_visible()) sh.dirty = 1;                  /* hidden: the app gets its space back */
        for (int i = 0; i < n; i++) key_event(&keys[i]);
        return;
    }
    case OWN_DOCK: dock_pointer(&e); return;
    case OWN_MAIN: break;
    default: return;
    }
    if (sh.launcher_open) { launcher_pointer(&e); return; }
    if (sh.view == VIEW_HOME) return;
    if (e.type == EV_DOWN && in_rect(back_rect(), e.x, e.y)) {
        shell_go_home();
        sh.owner = OWN_NONE;
        return;
    }
    sh.app_damaged = 0;
    if (sh.app->event && sh.app->event(&e, app_area()) && !sh.app_damaged) shell_damage(app_area());
}

/* ---- boot splash ------------------------------------------------------------- */
static void splash(float t) {
    canvas_t *c = &sh.scene;
    memcpy(c->px, sh.wall.px, (usize)ui.W * ui.H * 4);
    float cx = ui.W / 2.0f, cy = ui.H / 2.0f - dp(40);
    for (int i = 0; i < 3; i++) {
        float r = dp(34 + i * 20) * (0.6f + 0.4f * t);
        u32 col = ALPHA(i == 1 ? RGB(255, 255, 255) : ui.accent, (u32)(230 * t) / (u32)(i + 1));
        gfx_ring(c, cx, cy, r, dp(4), col);
    }
    gfx_circle(c, cx, cy, dp(12) * t, ui.accent);
    rect_t word = { 0, (int)cy + dp(110), ui.W, ui.h1->line };
    gfx_text_center(c, ui.h1, word, "QRT", ALPHA(ui.text, (u32)(255 * t)));
    rect_t sub = { 0, word.y + word.h, ui.W, ui.body->line };
    gfx_text_center(c, ui.body, sub, "Tessera kernel", ui.text2);
    int y = ui.H - dp(24) - ui.small->line * 8;
    int total = 0;
    while (klog_line(total)) total++;
    for (int i = MAX(0, total - 8); i < total; i++, y += ui.small->line)
        gfx_text_fit(c, ui.small, dp(24), y, ui.W - dp(48), klog_line(i), ui.text3);
    present(c, full_rect());
}

/* ---- frame pipeline ------------------------------------------------------------ */
static rect_t cursor_rect(int x, int y) { return (rect_t){ x - dp(3), y - dp(3), dp(20), dp(28) }; }

static void render(void) {
    rect_t pd = rect_intersect(rect_union(sh.dmg, sh.pdmg), full_rect());
    if (pd.w <= 0 || pd.h <= 0) { sh.dmg = sh.pdmg = (rect_t){ 0 }; return; }
    u64 t0 = k_now_us();
    if (sh.dmg.w > 0 && sh.dmg.h > 0) compose(sh.dmg);
    u64 t1 = k_now_us();
    if (sh.cursor_on) {
        for (int y = pd.y; y < pd.y + pd.h; y++)
            memcpy(sh.frame.px + (usize)y * ui.W + pd.x, sh.scene.px + (usize)y * ui.W + pd.x, (usize)pd.w * 4);
        gfx_limit(&sh.frame, pd);
        draw_cursor(&sh.frame, sh.cursor_x, sh.cursor_y);
        gfx_limit(&sh.frame, full_rect());
        present(&sh.frame, pd);
    } else {
        present(&sh.scene, pd);
    }
    u64 t2 = k_now_us();
    shell_stats.compose_us = (u32)(t1 - t0);
    shell_stats.present_us = (u32)(t2 - t1);
    shell_stats.area_permille = (u32)((u64)pd.w * pd.h * 1000 / ((u64)ui.W * ui.H));
    shell_stats.frames++;
    sh.dmg = sh.pdmg = (rect_t){ 0 };
}

static void run_benchmark(void) {
    rect_t full = full_rect();
    int side = dp(96);
    rect_t small = { (ui.W - side) / 2, (ui.H - side) / 2, side, side };
    force_single_core = 1;
    u64 s0 = k_now_us();
    for (int i = 0; i < 8; i++) compose(full);
    u64 single_us = (k_now_us() - s0) / 8;
    force_single_core = 0;
    s0 = k_now_us();
    for (int i = 0; i < 8; i++) compose(full);
    u64 multi_us = (k_now_us() - s0) / 8;
    u64 t0 = k_now_us();
    for (int i = 0; i < 8; i++) { compose(full); present(&sh.scene, full); }
    u64 t1 = k_now_us();
    for (int i = 0; i < 64; i++) { compose(small); present(&sh.scene, small); }
    u64 t2 = k_now_us();
    for (int i = 0; i < 8; i++) memcpy(sh.frame.px, sh.scene.px, (usize)ui.W * ui.H * 4);
    u64 t3 = k_now_us();
    for (int i = 0; i < 8; i++) present(&sh.scene, full);
    u64 t4 = k_now_us();
    u64 full_us = (t1 - t0) / 8, small_us = (t2 - t1) / 64, copy_us = (t3 - t2) / 8, blt_us = (t4 - t3) / 8;
    u64 mbps = copy_us ? (u64)ui.W * ui.H * 4 / copy_us : 0;    /* bytes/us == MB/s */
    fmt(shell_stats.bench[0], sizeof shell_stats.bench[0], "full frame %llu.%llu ms (%llu fps), of which present %llu.%llu ms",
        full_us / 1000, full_us / 100 % 10, full_us ? 1000000 / full_us : 0, blt_us / 1000, blt_us / 100 % 10);
    fmt(shell_stats.bench[1], sizeof shell_stats.bench[1], "96 dp partial redraw %llu.%llu ms (%llu fps)",
        small_us / 1000, small_us / 100 % 10, small_us ? 1000000 / small_us : 0);
    fmt(shell_stats.bench[2], sizeof shell_stats.bench[2], "frame copy %llu.%llu ms (%llu MB/s)",
        copy_us / 1000, copy_us / 100 % 10, mbps);
    fmt(shell_stats.bench[3], sizeof shell_stats.bench[3], "full draw: 1 core %llu.%llu ms, %d cores %llu.%llu ms (%llu.%llux)",
        single_us / 1000, single_us / 100 % 10, smp_workers() + (k.native ? 1 : 0) ? smp_workers() + (k.native ? 1 : 0) : 1, multi_us / 1000, multi_us / 100 % 10,
        multi_us ? single_us / multi_us : 0, multi_us ? single_us * 10 / multi_us % 10 : 0);
    for (int i = 0; i < 4; i++) klog("bench: %s", shell_stats.bench[i]);
    for (int i = 0; i < N_APPS; i++) if (apps[i] == &app_system) open_app(i);
    sh.dirty = 1;
}

/* ---- main loop ------------------------------------------------------------------ */
void shell_main(void) {
    sh.rot = (int)hal_setting_get(u"QrtRotation", 0) & 3;
    sh.accent_idx = (int)hal_setting_get(u"QrtAccent", 0) % N_ACCENTS;
    sh.dock_edge = (int)hal_setting_get(u"QrtDockEdge", DOCK_RIGHT) & 3;
    sh.sleep_after = (int)hal_setting_get(u"QrtSleepAfter", 120);
    sh.volume = sh.volume_saved = CLAMP((int)hal_setting_get(u"QrtVolume", 50), 0, 100);
    sh.launch_pressed = sh.dock_pressed = -1;
    k.graphics_up = 1;
    if (!k.native) k.st->ConOut->EnableCursor(k.st->ConOut, 0);
    relayout();

    for (int i = 1; i <= 12; i++) {
        float t = i / 12.0f;
        splash(1 - (1 - t) * (1 - t));
        hal_delay_us(25000);
    }
    k.boot_ms = k_now_ms();
    klog("shell: first frame after %llu ms", k.boot_ms);


    sh.dirty = 1;
    sh.last_input_ms = k_now_ms();
    for (;;) {
        hal_wait_frame();
        sh.now_ms = k_now_ms();

        event_t ev[32];
        int n = hal_poll(ev, 32);
        for (int i = 0; i < n; i++) dispatch(ev[i]);

        u64 now = k_now_ms();
        netstack_poll();
        if (sh.asleep) {
            /* the panel is dark: keep the network and the buttons going, draw nothing */
            for (int i = 0; i < 3; i++) hal_wait_frame();
            continue;
        }
        /* sleep after the configured idle time (sooner on the lock screen) */
        if (sh.sleep_after) {
            u64 limit = (u64)(sh.locked ? MIN(sh.sleep_after, 20) : sh.sleep_after) * 1000;
            if (now - sh.last_input_ms > limit) { go_to_sleep(); continue; }
        }
        dock_animate(now);
        if (sh.osd_shown && now > sh.osd_until) {
            sh.osd_shown = 0;
            shell_damage(osd_paint_rect());
            if (sh.volume != sh.volume_saved) { sh.volume_saved = sh.volume; hal_setting_set(u"QrtVolume", (u32)sh.volume); }
        }
        event_t rep[4];
        int nr = osk_tick(now, rep, 4);
        if (nr) shell_damage(osk_rect(content_rect()));
        for (int i = 0; i < nr; i++) key_event(&rep[i]);
        if (sh.bench_pending) { sh.bench_pending = 0; run_benchmark(); }
        if (sh.keyboard_pending) { sh.keyboard_pending = 0; shell_keyboard(!osk_visible()); }
        sh.app_damaged = 0;
        if (sh.view == VIEW_APP && sh.app->tick && sh.app->tick(now) && !sh.app_damaged) shell_damage(app_area());
        if (!sh.dirty) {
            EFI_TIME t;
            static u64 last_check;
            static char last_net[96];
            if (now - last_check > 1000) {
                last_check = now;
                k_walltime(&t);
                if (t.Minute != sh.last_minute) sh.dirty = 1;
                /* the home screen shows the network line */
                const char *ns = shell_net_status();
                if (strcmp(ns ? ns : "", last_net)) { strlcpy(last_net, ns ? ns : "", sizeof last_net); if (sh.view == VIEW_HOME) sh.dirty = 1; }
            }
        }
        if (sh.dirty) { sh.dmg = full_rect(); sh.dirty = 0; }
        if (sh.cursor_dirty) {
            /* repaint where the cursor was and where it is now */
            sh.cursor_dirty = 0;
            sh.pdmg = rect_union(sh.pdmg, sh.cursor_drawn);
            sh.cursor_drawn = sh.cursor_on ? cursor_rect(sh.cursor_x, sh.cursor_y) : (rect_t){ 0 };
            sh.pdmg = rect_union(sh.pdmg, sh.cursor_drawn);
        }
        render();
    }
}
