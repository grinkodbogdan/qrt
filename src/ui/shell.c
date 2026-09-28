/*
 * shell.c - QRT's user interface.
 *
 * Visual language borrows from Fuchsia's Armadillo/Ermine shells: a
 * wallpaper of soft light, glassy "story" cards, a big quiet clock and an
 * Ask bar that doubles as launcher and command line.  Everything is drawn
 * in software into a logical canvas which is rotated (for tablets held in
 * either orientation) and pushed to the panel through GOP.
 */
#include "shell.h"
#include "../kernel/smp.h"

ui_t ui;

const u32 accent_palette[N_ACCENTS] = {
    RGB(0xff, 0x4f, 0xa3), RGB(0x7c, 0x6c, 0xff), RGB(0x2e, 0xc4, 0xb6),
    RGB(0xff, 0x8a, 0x4c), RGB(0x4d, 0xa3, 0xff), RGB(0xa6, 0xe2, 0x2e),
};
const char *accent_names[N_ACCENTS] = { "Fuchsia", "Iris", "Lagoon", "Ember", "Sky", "Lime" };

static const app_t *apps[] = { &app_clock, &app_sketch, &app_files, &app_system, &app_settings, &app_life, &app_lab };
#define N_APPS ((int)ARRAY_LEN(apps))

typedef enum { VIEW_HOME, VIEW_APP } view_t;

static struct {
    canvas_t scene, frame, phys, wall, wall_dim;
    int rot, accent_idx;
    view_t view;
    const app_t *app;
    int dirty;
    int focus, pressed;           /* home grid */
    tap_t tap;
    int ask_open;
    char query[48];
    int ask_pressed;
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

static rect_t ask_bar_rect(void) {
    int w = MIN(ui.W - dp(40), dp(560)), h = dp(52);
    return (rect_t){ (ui.W - w) / 2, ui.H - dp(28) - h, w, h };
}

static void draw_ask_bar(canvas_t *c) {
    rect_t r = ask_bar_rect();
    gfx_shadow(c, r, r.h / 2, dp(18), RGBA(0, 0, 0, 90));
    gfx_rrect(c, r, r.h / 2, RGBA(0xf5, 0xf3, 0xfa, 235));
    float cx = r.x + r.h / 2.0f + dp(6), cy = r.y + r.h / 2.0f;
    gfx_ring(c, cx, cy, dp(11), dp(3), ui.accent);
    gfx_circle(c, cx, cy, dp(4), ui.accent);
    gfx_text(c, ui.body, r.x + r.h + dp(8), r.y + (r.h - ui.body->line) / 2, "Ask for anything", RGBA(0x20, 0x1a, 0x30, 150));
}

/* ---- home ----------------------------------------------------------------- */
static void grid_geometry(rect_t *area, int *cols, int *cw, int *ch, int *gap) {
    int pad = dp(20);
    *gap = dp(14);
    *cols = ui.landscape ? 3 : 2;
    int top = ui.landscape ? dp(40) + ui.huge->line + dp(70) : dp(64) + ui.huge->line + dp(96);
    rect_t ask = ask_bar_rect();
    int avail_h = ask.y - dp(28) - top;
    int rows = (N_APPS + *cols - 1) / *cols;
    *cw = (ui.W - 2 * pad - (*cols - 1) * *gap) / *cols;
    *ch = MIN(dp(150), (avail_h - (rows - 1) * *gap) / rows);
    /* in landscape, the grid sits in the right-hand column beside the clock */
    area->x = pad; area->y = top;
    area->w = ui.W - 2 * pad;
    area->h = rows * *ch + (rows - 1) * *gap;
    if (ui.landscape) {
        area->x = ui.W / 2 - dp(10);
        area->w = ui.W - area->x - pad;
        *cols = 2;
        rows = (N_APPS + 1) / 2;
        area->y = dp(56);
        *cw = (area->w - *gap) / 2;
        *ch = MIN(dp(150), (ask.y - dp(28) - area->y - (rows - 1) * *gap) / rows);
        area->h = rows * *ch + (rows - 1) * *gap;
    }
}

static rect_t card_rect(int i) {
    rect_t area; int cols, cw, ch, gap;
    grid_geometry(&area, &cols, &cw, &ch, &gap);
    return (rect_t){ area.x + (i % cols) * (cw + gap), area.y + (i / cols) * (ch + gap), cw, ch };
}

static void draw_home(canvas_t *c, const EFI_TIME *t) {
    int pad = dp(24);
    char buf[64];
    int y = ui.landscape ? dp(56) : dp(64);

    clock_text(buf, sizeof buf, t);
    gfx_text(c, ui.huge, pad - dp(4), y, buf, ui.text);
    y += ui.huge->line;
    fmt(buf, sizeof buf, "%s, %d %s", weekday(t), t->Day, month_name(t->Month));
    gfx_text(c, ui.title, pad, y, buf, ui.text);
    y += ui.title->line + dp(6);
    const char *greet = t->Hour < 5 ? "Up late" : t->Hour < 12 ? "Good morning" : t->Hour < 18 ? "Good afternoon" : "Good evening";
    fmt(buf, sizeof buf, "%s. Everything runs on your firmware.", greet);
    gfx_text_fit(c, ui.body, pad, y, (ui.landscape ? ui.W / 2 - dp(40) : ui.W - 2 * pad), buf, ui.text2);

    {
        /* hardware summary: under the greeting in landscape, under the grid in portrait */
        rect_t ga; int gc, gw, gh, gg;
        grid_geometry(&ga, &gc, &gw, &gh, &gg);
        int yy = ui.landscape ? y + ui.body->line + dp(28) : ga.y + ga.h + dp(24);
        char ram[24], line[96];
        fmt_bytes(ram, sizeof ram, k.ram_bytes);
        fmt(line, sizeof line, "%s  \xc2\xb7  %s RAM", k.cpu, ram);
        int colw = ui.landscape ? ui.W / 2 - dp(40) : ui.W - 2 * pad;
        gfx_text_fit(c, ui.small, pad, yy, colw, line, ui.text3);
        fmt(line, sizeof line, "%d touch  \xc2\xb7  %d pointer  \xc2\xb7  %d volume%s",
            k.n_abs, k.n_rel, k.n_vol, k.n_vol == 1 ? "" : "s");
        gfx_text(c, ui.small, pad, yy + ui.small->line, line, ui.text3);
    }

    rect_t area; int cols, cw, ch, gap;
    grid_geometry(&area, &cols, &cw, &ch, &gap);
    if (!ui.landscape) ui_section(c, area.x + dp(4), area.y - ui.small->line - dp(8), "STORIES");
    for (int i = 0; i < N_APPS; i++) {
        const app_t *a = apps[i];
        rect_t r = card_rect(i);
        int rad = dp(22);
        ui_card(c, r, rad, sh.pressed == i);
        if (sh.focus == i) gfx_rrect_outline(c, r, rad, dp(2), ui.accent);
        float ir = (float)MIN(dp(22), r.h / 5);
        float icx = r.x + dp(18) + ir, icy = r.y + dp(18) + ir;
        gfx_circle(c, icx, icy, ir, a->color);
        a->icon(c, icx, icy, ir * 0.62f, RGB(255, 255, 255));
        int ty = r.y + r.h - dp(16) - ui.small->line - ui.label->line;
        gfx_text_fit(c, ui.label, r.x + dp(18), ty, r.w - dp(30), a->name, ui.text);
        gfx_text_fit(c, ui.small, r.x + dp(18), ty + ui.label->line, r.w - dp(30), a->blurb, ui.text2);
    }
    draw_ask_bar(c);
}

/* ---- ask (launcher + command line) ---------------------------------------- */
typedef struct { const char *title, *sub; int app; int action; } suggestion_t;
enum { ACT_NONE, ACT_SHUTDOWN, ACT_REBOOT, ACT_FIRMWARE, ACT_ROTATE, ACT_ACCENT,
       ACT_TOUCH_SWAP, ACT_TOUCH_FLIPX, ACT_TOUCH_FLIPY, ACT_TOUCH_RESET, ACT_BENCH, ACT_SMP };

static int suggestions(suggestion_t *out, int max) {
    static const suggestion_t actions[] = {
        { "Rotate screen", "Turn the canvas 90\xc2\xb0", -1, ACT_ROTATE },
        { "Next accent colour", "Cycle the theme", -1, ACT_ACCENT },
        { "Restart", "Cold reset through UEFI", -1, ACT_REBOOT },
        { "Shut down", "Power off through UEFI", -1, ACT_SHUTDOWN },
        { "Firmware setup", "Reboot into the BIOS/UEFI menu", -1, ACT_FIRMWARE },
        { "Graphics benchmark", "Time full and partial redraws", -1, ACT_BENCH },
        { "Multicore rendering on/off", "Draw with the other CPU cores", -1, ACT_SMP },
        { "Touch: swap axes", "Fix a touchscreen mounted sideways", -1, ACT_TOUCH_SWAP },
        { "Touch: flip X", "Mirror touch left-right", -1, ACT_TOUCH_FLIPX },
        { "Touch: flip Y", "Mirror touch top-bottom", -1, ACT_TOUCH_FLIPY },
        { "Touch: reset", "Use the firmware's mapping as-is", -1, ACT_TOUCH_RESET },
    };
    int n = 0;
    for (int i = 0; i < N_APPS && n < max; i++)
        if (str_icontains(apps[i]->name, sh.query) || str_icontains(apps[i]->blurb, sh.query))
            out[n++] = (suggestion_t){ apps[i]->name, apps[i]->blurb, i, ACT_NONE };
    for (int i = 0; i < (int)ARRAY_LEN(actions) && n < max; i++)
        if (str_icontains(actions[i].title, sh.query) || str_icontains(actions[i].sub, sh.query))
            out[n++] = actions[i];
    return n;
}

static rect_t ask_panel_rect(int n) {
    rect_t bar = ask_bar_rect();
    int row = dp(56);
    int h = dp(76) + n * row + dp(12);
    h = MIN(h, ui.H - dp(80));
    return (rect_t){ bar.x, bar.y + bar.h - h, bar.w, h };
}

static rect_t ask_row_rect(rect_t panel, int i) {
    return (rect_t){ panel.x + dp(10), panel.y + dp(70) + i * dp(56), panel.w - dp(20), dp(52) };
}

static void draw_ask(canvas_t *c) {
    suggestion_t s[16];
    int n = suggestions(s, 16);
    gfx_fill(c, (rect_t){ 0, 0, ui.W, ui.H }, RGBA(0, 0, 0, 120));
    rect_t p = ask_panel_rect(n);
    gfx_shadow(c, p, dp(26), dp(24), RGBA(0, 0, 0, 120));
    gfx_rrect(c, p, dp(26), RGB(0xf5, 0xf3, 0xfa));

    rect_t field = { p.x + dp(12), p.y + dp(12), p.w - dp(24), dp(48) };
    gfx_rrect(c, field, field.h / 2, RGB(0xe8, 0xe4, 0xf0));
    float cx = field.x + dp(26), cy = field.y + field.h / 2.0f;
    gfx_ring(c, cx, cy, dp(10), dp(3), ui.accent);
    gfx_circle(c, cx, cy, dp(4), ui.accent);
    int tx = field.x + dp(48), ty = field.y + (field.h - ui.body->line) / 2;
    int end = sh.query[0] ? gfx_text(c, ui.body, tx, ty, sh.query, RGB(0x20, 0x1a, 0x30))
                          : (gfx_text(c, ui.body, tx, ty, "Type, or tap a suggestion", RGBA(0x20, 0x1a, 0x30, 120)), tx);
    gfx_fill(c, (rect_t){ end + 1, ty + dp(2), MAX(1, dp(2)), ui.body->line - dp(4) }, ui.accent);

    for (int i = 0; i < n; i++) {
        rect_t r = ask_row_rect(p, i);
        if (r.y + r.h > p.y + p.h) break;
        if (sh.ask_pressed == i || (i == 0 && sh.query[0])) gfx_rrect(c, r, dp(14), RGBA(0x7c, 0x6c, 0xff, 30));
        u32 dot = s[i].app >= 0 ? apps[s[i].app]->color : RGB(0x55, 0x50, 0x66);
        float icx = r.x + dp(24), icy = r.y + r.h / 2.0f;
        gfx_circle(c, icx, icy, dp(15), dot);
        if (s[i].app >= 0) apps[s[i].app]->icon(c, icx, icy, dp(9), RGB(255, 255, 255));
        else gfx_ring(c, icx, icy, dp(6), dp(2), RGB(255, 255, 255));
        gfx_text(c, ui.label, r.x + dp(50), r.y + dp(6), s[i].title, RGB(0x1a, 0x16, 0x28));
        gfx_text_fit(c, ui.small, r.x + dp(50), r.y + dp(6) + ui.label->line, r.w - dp(60), s[i].sub, RGBA(0x1a, 0x16, 0x28, 150));
    }
}

/* ---- app chrome ------------------------------------------------------------- */
static rect_t app_area(void) { return (rect_t){ 0, dp(96), ui.W, ui.H - dp(96) - dp(28) }; }
rect_t shell_app_area(void) { return app_area(); }
static rect_t back_rect(void) { return (rect_t){ dp(14), dp(40), dp(48), dp(48) }; }
static rect_t home_zone(void) { return (rect_t){ 0, ui.H - dp(28), ui.W, dp(28) }; }

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

    rect_t hz = home_zone();
    int hw = dp(120);
    gfx_rrect(c, (rect_t){ (ui.W - hw) / 2, hz.y + hz.h / 2 - dp(2), hw, dp(5) }, dp(3), RGBA(255, 255, 255, 170));
}

/* ---- compose ---------------------------------------------------------------- */
/*
 * Redraw only rectangle d of the scene.  Everything is drawn as before but
 * with d as the canvas limit, so primitives outside it are rejected by their
 * clip test and the cost scales with the damaged area, not the screen.
 */
static EFI_TIME frame_time;           /* wall clock sampled once per frame on the boot core */
void shell_time(EFI_TIME *t) { *t = frame_time; }

/* Draw rectangle d into the scene.  Runs on any core: it touches only pixels
 * inside d and uses a private copy of the canvas's clip state. */
static void compose_rect(rect_t d) {
    canvas_t c = sh.scene;
    gfx_limit(&c, d);
    d = c.limit;
    const canvas_t *wall = sh.view == VIEW_APP ? &sh.wall_dim : &sh.wall;
    for (int y = d.y; y < d.y + d.h; y++)
        memcpy(c.px + (usize)y * c.stride + d.x, wall->px + (usize)y * wall->stride + d.x, (usize)d.w * 4);
    draw_status(&c, &frame_time);
    if (sh.view == VIEW_HOME) draw_home(&c, &frame_time);
    else draw_app(&c);
    if (sh.ask_open) draw_ask(&c);
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
    sh.ask_open = 0;
    sh.pressed = -1;
    if (sh.app->open) sh.app->open();
    sh.dirty = 1;
}

void shell_go_home(void) {
    sh.view = VIEW_HOME;
    sh.app = NULL;
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
    }
}

static void activate(const suggestion_t *s) {
    sh.ask_open = 0;
    sh.query[0] = 0;
    if (s->app >= 0) open_app(s->app);
    else run_action(s->action);
    sh.dirty = 1;
}

static void ask_key(const event_t *e) {
    usize n = strlen(sh.query);
    suggestion_t s[16];
    if (e->scan == SCAN_ESC) { sh.ask_open = 0; sh.query[0] = 0; }
    else if (e->ch == '\r' || e->ch == '\n') { if (suggestions(s, 16) > 0) activate(&s[0]); }
    else if (e->ch == 8) { if (n) sh.query[n - 1] = 0; else sh.ask_open = 0; }
    else if (e->ch >= 32 && e->ch < 127 && n + 1 < sizeof sh.query) { sh.query[n] = (char)e->ch; sh.query[n + 1] = 0; }
    sh.dirty = 1;
}

static void ask_pointer(const event_t *e) {
    suggestion_t s[16];
    int n = suggestions(s, 16);
    rect_t p = ask_panel_rect(n);
    int hit = -1;
    for (int i = 0; i < n; i++) {
        rect_t r = ask_row_rect(p, i);
        if (r.y + r.h <= p.y + p.h && in_rect(r, e->x, e->y)) hit = i;
    }
    if (e->type == EV_DOWN) { sh.ask_pressed = hit; sh.dirty = 1; }
    if (tap_track(&sh.tap, e, dp(10))) {
        if (hit >= 0 && hit == sh.ask_pressed) activate(&s[hit]);
        else if (!in_rect(p, e->x, e->y)) { sh.ask_open = 0; sh.query[0] = 0; }
        sh.ask_pressed = -1;
        sh.dirty = 1;
    }
    if (e->type == EV_UP) { sh.ask_pressed = -1; sh.dirty = 1; }
}

static void home_pointer(const event_t *e) {
    int hit = -1;
    for (int i = 0; i < N_APPS; i++) if (in_rect(card_rect(i), e->x, e->y)) hit = i;
    if (e->type == EV_DOWN) { sh.pressed = hit; sh.focus = -1; sh.dirty = 1; }
    if (e->type == EV_MOVE && sh.tap.down && sh.pressed != hit && sh.pressed >= 0) { sh.pressed = -1; sh.dirty = 1; }
    int tapped = tap_track(&sh.tap, e, dp(12));
    if (tapped) {
        if (hit >= 0 && hit == sh.pressed) open_app(hit);
        else if (in_rect(ask_bar_rect(), e->x, e->y)) { sh.ask_open = 1; sh.ask_pressed = -1; }
    }
    if (e->type == EV_UP) { sh.pressed = -1; sh.dirty = 1; }
}

static void home_key(const event_t *e) {
    int cols = 2;
    if (e->ch >= 32 && e->ch < 127) { sh.ask_open = 1; sh.query[0] = 0; ask_key(e); return; }
    if (sh.focus < 0) { sh.focus = 0; sh.dirty = 1; return; }
    switch (e->scan) {
    case SCAN_RIGHT: sh.focus = (sh.focus + 1) % N_APPS; break;
    case SCAN_LEFT:  sh.focus = (sh.focus + N_APPS - 1) % N_APPS; break;
    case SCAN_DOWN:  sh.focus = MIN(sh.focus + cols, N_APPS - 1); break;
    case SCAN_UP:    sh.focus = MAX(sh.focus - cols, 0); break;
    }
    if (e->ch == '\r' || e->ch == ' ') open_app(sh.focus);
    sh.dirty = 1;
}

static void dispatch(event_t e) {
    if (e.type == EV_DOWN || e.type == EV_MOVE || e.type == EV_UP || e.type == EV_SCROLL) {
        int lx, ly;
        to_logical(e.x, e.y, &lx, &ly);
        e.x = lx; e.y = ly;
        if (e.from_mouse) { sh.cursor_x = lx; sh.cursor_y = ly; sh.cursor_on = 1; sh.cursor_dirty = 1; }
        else if (sh.cursor_on) { sh.cursor_on = 0; sh.cursor_dirty = 1; }
    }
    if (sh.ask_open) {
        if (e.type == EV_KEY) ask_key(&e); else ask_pointer(&e);
        return;
    }
    if (sh.view == VIEW_HOME) {
        if (e.type == EV_KEY) home_key(&e); else home_pointer(&e);
        return;
    }
    /* app view: chrome first, then the app */
    if (e.type == EV_KEY && e.scan == SCAN_ESC) { shell_go_home(); return; }
    if (e.type == EV_DOWN && (in_rect(back_rect(), e.x, e.y) || in_rect(home_zone(), e.x, e.y))) {
        shell_go_home();
        sh.tap.down = 0;
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
    gfx_text_center(c, ui.body, sub, "Tessera kernel \xc2\xb7 firmware-hosted", ui.text2);
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
    sh.focus = -1; sh.pressed = -1; sh.ask_pressed = -1;
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
    for (;;) {
        hal_wait_frame();

        event_t ev[32];
        int n = hal_poll(ev, 32);
        for (int i = 0; i < n; i++) dispatch(ev[i]);

        u64 now = k_now_ms();
        if (sh.bench_pending) { sh.bench_pending = 0; run_benchmark(); }
        sh.app_damaged = 0;
        if (sh.view == VIEW_APP && sh.app->tick && sh.app->tick(now) && !sh.app_damaged) shell_damage(app_area());
        if (!sh.dirty) {
            EFI_TIME t;
            static u64 last_check;
            if (now - last_check > 1000) {
                last_check = now;
                k_walltime(&t);
                if (t.Minute != sh.last_minute) sh.dirty = 1;
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
