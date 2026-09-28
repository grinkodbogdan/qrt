/* gfx.h - software 2D: anti-aliased shapes and text on 32-bit canvases. */
#pragma once
#include "../kernel/rt.h"
#include "font.h"

/* colours are 0xAARRGGBB (the GOP BLT pixel layout plus alpha) */
#define RGB(r, g, b)      (0xff000000u | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define RGBA(r, g, b, a)  (((u32)(a) << 24) | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define ALPHA(c, a)       (((c) & 0xffffffu) | ((u32)(a) << 24))

typedef struct { int x, y, w, h; } rect_t;

typedef struct {
    u32 *px;
    int w, h, stride;
    rect_t clip;
    rect_t limit;      /* outer bound for gfx_clip(): the region being redrawn */
} canvas_t;

typedef enum { F_REGULAR, F_SEMIBOLD, F_LIGHT } face_t;

canvas_t canvas_new(int w, int h);
void     canvas_free(canvas_t *c);
void     gfx_clip(canvas_t *c, rect_t r);
void     gfx_unclip(canvas_t *c);
void     gfx_limit(canvas_t *c, rect_t r);   /* restrict all drawing (partial redraw) */
rect_t   rect_union(rect_t a, rect_t b);      /* empty rects are ignored */
rect_t   rect_intersect(rect_t a, rect_t b);

void gfx_fill(canvas_t *c, rect_t r, u32 color);             /* alpha-blended */
void gfx_rrect(canvas_t *c, rect_t r, int radius, u32 color);
void gfx_rrect_outline(canvas_t *c, rect_t r, int radius, int width, u32 color);
void gfx_shadow(canvas_t *c, rect_t r, int radius, int spread, u32 color);
void gfx_circle(canvas_t *c, float cx, float cy, float rad, u32 color);
void gfx_ring(canvas_t *c, float cx, float cy, float rad, float width, u32 color);
void gfx_line(canvas_t *c, float x0, float y0, float x1, float y1, float width, u32 color);
void gfx_vgradient(canvas_t *c, rect_t r, u32 top, u32 bottom);
void gfx_blit(canvas_t *dst, int dx, int dy, const canvas_t *src);
void gfx_blit_rounded(canvas_t *dst, int dx, int dy, const canvas_t *src, int radius);

const font_t *font_pick(face_t face, int px);
int  text_width(const font_t *f, const char *s);
int  gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *s, u32 color); /* y = line top */
void gfx_text_center(canvas_t *c, const font_t *f, rect_t r, const char *s, u32 color);
int  gfx_text_fit(canvas_t *c, const font_t *f, int x, int y, int maxw, const char *s, u32 color);

u32  mix(u32 a, u32 b, int t255);
static inline int in_rect(rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
