/* gfx.c - anti-aliased software rasteriser. */
#include "gfx.h"

canvas_t canvas_new(int w, int h) {
    canvas_t c = { kalloc((usize)w * h * 4), w, h, w, { 0, 0, w, h }, { 0, 0, w, h } };
    return c;
}

void canvas_free(canvas_t *c) { kfree(c->px); c->px = NULL; }

static rect_t intersect(rect_t a, rect_t b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    rect_t r = { x0, y0, MAX(0, x1 - x0), MAX(0, y1 - y0) };
    return r;
}

void gfx_clip(canvas_t *c, rect_t r) { c->clip = intersect(c->limit, r); }
void gfx_unclip(canvas_t *c) { c->clip = c->limit; }
void gfx_limit(canvas_t *c, rect_t r) {
    rect_t full = { 0, 0, c->w, c->h };
    c->limit = c->clip = intersect(full, r);
}
rect_t rect_intersect(rect_t a, rect_t b) { return intersect(a, b); }
rect_t rect_union(rect_t a, rect_t b) {
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

u32 mix(u32 a, u32 b, int t) {
    u32 rb = ((a & 0xff00ff) * (255 - t) + (b & 0xff00ff) * t) >> 8;
    u32 g  = ((a & 0x00ff00) * (255 - t) + (b & 0x00ff00) * t) >> 8;
    u32 al = (((a >> 24) * (255 - t) + (b >> 24) * t) / 255) << 24;
    return al | (rb & 0xff00ff) | (g & 0x00ff00);
}

/* blend colour with coverage a (0..255, already multiplied by colour alpha) */
static inline void blend(u32 *d, u32 color, u32 a) {
    if (a >= 255) { *d = color | 0xff000000u; return; }
    if (!a) return;
    u32 dst = *d;
    u32 rb = ((color & 0xff00ff) * a + (dst & 0xff00ff) * (255 - a)) >> 8;
    u32 g  = ((color & 0x00ff00) * a + (dst & 0x00ff00) * (255 - a)) >> 8;
    *d = 0xff000000u | (rb & 0xff00ff) | (g & 0x00ff00);
}

void gfx_fill(canvas_t *c, rect_t r, u32 color) {
    r = intersect(r, c->clip);
    u32 a = color >> 24;
    for (int y = r.y; y < r.y + r.h; y++) {
        u32 *row = c->px + (usize)y * c->stride + r.x;
        if (a == 255) for (int x = 0; x < r.w; x++) row[x] = color;
        else for (int x = 0; x < r.w; x++) blend(&row[x], color, a);
    }
}

void gfx_vgradient(canvas_t *c, rect_t r, u32 top, u32 bottom) {
    rect_t cl = intersect(r, c->clip);
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        int t = r.h > 1 ? (y - r.y) * 255 / (r.h - 1) : 0;
        rect_t row = { cl.x, y, cl.w, 1 };
        gfx_fill(c, row, mix(top, bottom, t));
    }
}

void gfx_blit(canvas_t *dst, int dx, int dy, const canvas_t *src) {
    rect_t r = intersect((rect_t){ dx, dy, src->w, src->h }, dst->clip);
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(dst->px + (usize)y * dst->stride + r.x,
               src->px + (usize)(y - dy) * src->stride + (r.x - dx), (usize)r.w * 4);
}

/* Signed distance from pixel centre to a rounded rectangle's edge. */
static inline float rrect_sd(rect_t r, float rad, float px, float py) {
    float cx = CLAMP(px, r.x + rad, r.x + r.w - rad);
    float cy = CLAMP(py, r.y + rad, r.y + r.h - rad);
    float dx = px - cx, dy = py - cy;
    if (dx == 0 && dy == 0) {
        /* inside the straight part: distance to nearest edge, negative */
        float e = MIN(MIN(px - r.x, r.x + r.w - px), MIN(py - r.y, r.y + r.h - py));
        return -e;
    }
    return fsqrt(dx * dx + dy * dy) - rad;
}

void gfx_blit_rounded(canvas_t *dst, int dx, int dy, const canvas_t *src, int radius) {
    rect_t full = { dx, dy, src->w, src->h };
    rect_t r = intersect(full, dst->clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        u32 *d = dst->px + (usize)y * dst->stride;
        const u32 *s = src->px + (usize)(y - dy) * src->stride - dx;
        int corner_row = y < dy + radius || y >= dy + src->h - radius;
        if (!corner_row) { memcpy(d + r.x, s + r.x, (usize)r.w * 4); continue; }
        for (int x = r.x; x < r.x + r.w; x++) {
            float sd = rrect_sd(full, (float)radius, x + 0.5f, y + 0.5f);
            float cov = CLAMP(0.5f - sd, 0.0f, 1.0f);
            blend(&d[x], s[x], (u32)(cov * 255));
        }
    }
}

void gfx_rrect(canvas_t *c, rect_t r, int radius, u32 color) {
    radius = MIN(radius, MIN(r.w, r.h) / 2);
    if (radius <= 0) { gfx_fill(c, r, color); return; }
    rect_t cl = intersect(r, c->clip);
    u32 a = color >> 24;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        int corner_row = y < r.y + radius || y >= r.y + r.h - radius;
        u32 *row = c->px + (usize)y * c->stride;
        if (!corner_row) {
            rect_t span = { cl.x, y, cl.w, 1 };
            gfx_fill(c, span, color);
            continue;
        }
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            int corner_col = x < r.x + radius || x >= r.x + r.w - radius;
            if (!corner_col) { blend(&row[x], color, a); continue; }
            float d = rrect_sd(r, (float)radius, x + 0.5f, y + 0.5f);
            float cov = CLAMP(0.5f - d, 0.0f, 1.0f);
            blend(&row[x], color, (u32)(cov * a));
        }
    }
}

void gfx_rrect_outline(canvas_t *c, rect_t r, int radius, int width, u32 color) {
    radius = MIN(radius, MIN(r.w, r.h) / 2);
    rect_t cl = intersect(r, c->clip);
    u32 a = color >> 24;
    float rad = (float)radius;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        u32 *row = c->px + (usize)y * c->stride;
        int mid_row = y >= r.y + MAX(radius, width) && y < r.y + r.h - MAX(radius, width);
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            if (mid_row && x >= r.x + width + 1 && x < r.x + r.w - width - 1) {
                x = r.x + r.w - width - 2;   /* jump over the hollow middle */
                continue;
            }
            float d = rrect_sd(r, rad, x + 0.5f, y + 0.5f);
            float cov = CLAMP(0.5f - d, 0.0f, 1.0f) - CLAMP(0.5f - (d + width), 0.0f, 1.0f);
            if (cov > 0) blend(&row[x], color, (u32)(cov * a));
        }
    }
}

void gfx_shadow(canvas_t *c, rect_t r, int radius, int spread, u32 color) {
    rect_t box = { r.x - spread, r.y - spread, r.w + 2 * spread, r.h + 2 * spread };
    rect_t cl = intersect(box, c->clip);
    u32 a = color >> 24;
    float rad = (float)radius;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        u32 *row = c->px + (usize)y * c->stride;
        int mid_row = y >= r.y + radius && y < r.y + r.h - radius;
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            if (mid_row && x >= r.x && x < r.x + r.w) { x = r.x + r.w - 1; continue; }
            float d = rrect_sd(r, rad, x + 0.5f, y + 0.5f);
            if (d <= 0 || d >= spread) continue;
            float t = 1.0f - d / spread;
            blend(&row[x], color, (u32)(t * t * a));
        }
    }
}

void gfx_circle(canvas_t *c, float cx, float cy, float rad, u32 color) {
    rect_t box = { (int)(cx - rad - 1), (int)(cy - rad - 1), (int)(2 * rad + 3), (int)(2 * rad + 3) };
    rect_t cl = intersect(box, c->clip);
    u32 a = color >> 24;
    float inner = rad - 0.8f;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        u32 *row = c->px + (usize)y * c->stride;
        float dy = y + 0.5f - cy;
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            float dx = x + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 < inner * inner) { blend(&row[x], color, a); continue; }
            float cov = CLAMP(rad + 0.5f - fsqrt(d2), 0.0f, 1.0f);
            if (cov > 0) blend(&row[x], color, (u32)(cov * a));
        }
    }
}

void gfx_ring(canvas_t *c, float cx, float cy, float rad, float width, u32 color) {
    rect_t box = { (int)(cx - rad - 1), (int)(cy - rad - 1), (int)(2 * rad + 3), (int)(2 * rad + 3) };
    rect_t cl = intersect(box, c->clip);
    u32 a = color >> 24;
    float hw = width / 2, mid = rad - hw;
    float skip = mid - hw - 1;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        u32 *row = c->px + (usize)y * c->stride;
        float dy = y + 0.5f - cy;
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            float dx = x + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (skip > 0 && d2 < skip * skip) continue;
            float d = fsqrt(d2) - mid;
            if (d < 0) d = -d;
            float cov = CLAMP(hw + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0) blend(&row[x], color, (u32)(cov * a));
        }
    }
}

void gfx_line(canvas_t *c, float x0, float y0, float x1, float y1, float width, u32 color) {
    float hw = width / 2;
    int bx0 = (int)(MIN(x0, x1) - hw - 1), by0 = (int)(MIN(y0, y1) - hw - 1);
    int bx1 = (int)(MAX(x0, x1) + hw + 2), by1 = (int)(MAX(y0, y1) + hw + 2);
    rect_t cl = intersect((rect_t){ bx0, by0, bx1 - bx0, by1 - by0 }, c->clip);
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    u32 a = color >> 24;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        u32 *row = c->px + (usize)y * c->stride;
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? CLAMP((px * vx + py * vy) / len2, 0.0f, 1.0f) : 0;
            float dx = px - t * vx, dy = py - t * vy;
            float d2 = dx * dx + dy * dy;
            if (d2 > (hw + 1) * (hw + 1)) continue;
            float cov = CLAMP(hw + 0.5f - fsqrt(d2), 0.0f, 1.0f);
            if (cov > 0) blend(&row[x], color, (u32)(cov * a));
        }
    }
}

/* ---- text ---------------------------------------------------------------- */
const font_t *font_pick(face_t face, int px) {
    const font_t *f = face == F_LIGHT ? font_light : face == F_SEMIBOLD ? font_semibold :
                      face == F_MONO ? font_mono : font_regular;
    const font_t *best = f;
    for (; f->size; f++)
        if (f->size <= px || (best->size > px && f->size < best->size)) best = f;
    return best;
}

static const glyph_t *glyph(const font_t *f, u32 cp) {
    if (f->count >= 95 && cp >= 32 && cp < 127) return &f->g[cp - 32];
    for (int i = 0; i < f->count; i++)
        if (f->g[i].cp == cp) return &f->g[i];
    return cp == '?' ? NULL : glyph(f, '?');
}

static u32 next_cp(const char **s) {
    const u8 *p = (const u8 *)*s;
    u32 c = *p++;
    if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
    else if (c >= 0xe0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if (c >= 0xc0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}

int text_width(const font_t *f, const char *s) {
    int w = 0;
    while (*s) {
        const glyph_t *g = glyph(f, next_cp(&s));
        if (g) w += g->adv64;
    }
    return (w + 32) >> 6;
}

int gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *s, u32 color) {
    int pen = x << 6;
    u32 a = color >> 24;
    while (*s) {
        const glyph_t *g = glyph(f, next_cp(&s));
        if (!g) continue;
        int gx = ((pen + 32) >> 6) + g->bx, gy = y + g->by;
        rect_t box = intersect((rect_t){ gx, gy, g->w, g->h }, c->clip);
        const u8 *bits = f->bits + g->off;
        for (int yy = box.y; yy < box.y + box.h; yy++) {
            const u8 *src = bits + (yy - gy) * g->w + (box.x - gx);
            u32 *dst = c->px + (usize)yy * c->stride + box.x;
            for (int xx = 0; xx < box.w; xx++)
                if (src[xx]) blend(&dst[xx], color, src[xx] * a / 255);
        }
        pen += g->adv64;
    }
    return (pen + 32) >> 6;
}

void gfx_text_center(canvas_t *c, const font_t *f, rect_t r, const char *s, u32 color) {
    int w = text_width(f, s);
    gfx_text(c, f, r.x + (r.w - w) / 2, r.y + (r.h - f->line) / 2, s, color);
}

/* draw text, truncating with an ellipsis if it would exceed maxw */
int gfx_text_fit(canvas_t *c, const font_t *f, int x, int y, int maxw, const char *s, u32 color) {
    if (text_width(f, s) <= maxw) return gfx_text(c, f, x, y, s, color);
    char buf[160];
    usize n = MIN(strlen(s), sizeof buf - 4);
    memcpy(buf, s, n);
    while (n > 0) {
        do n--; while (n > 0 && ((u8)buf[n] & 0xc0) == 0x80);
        memcpy(buf + n, "\xe2\x80\xa6", 4);   /* U+2026 ellipsis */
        if (text_width(f, buf) <= maxw) break;
    }
    return gfx_text(c, f, x, y, buf, color);
}
