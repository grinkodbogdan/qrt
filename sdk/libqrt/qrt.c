/* qrt.c - libqrt: QRT's own system calls (sdk/syscalls.txt, 1024 and up) and a few
 * drawing helpers for native programs. */
#include "qrt.h"
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

#define QRT_SYS_WINDOW_CREATE  1024
#define QRT_SYS_WINDOW_MAP     1025
#define QRT_SYS_WINDOW_PRESENT 1026
#define QRT_SYS_WINDOW_CLOSE   1027
#define QRT_SYS_EVENT_WAIT     1028
#define QRT_SYS_WINDOW_TITLE   1029
#define QRT_SYS_KEYBOARD       1030
#define QRT_SYS_EVENT_FD       1031

static long sc(long n, long a, long b, long c, long d, long e) {
    long r = syscall(n, a, b, c, d, e);
    return r < 0 ? -errno : r;
}

int qrt_window_create(const char *title) { return (int)sc(QRT_SYS_WINDOW_CREATE, (long)title, 0, 0, 0, 0); }

int qrt_window_buffer(int win, qrt_buffer *b) {
    struct { uint64_t addr; int32_t w, h, stride, pad; } info;
    long r = sc(QRT_SYS_WINDOW_MAP, win, (long)&info, 0, 0, 0);
    if (r < 0) return (int)r;
    b->px = (uint32_t *)(uintptr_t)info.addr;
    b->w = info.w; b->h = info.h; b->stride = info.stride;
    return 0;
}

int qrt_window_present(int win, int x, int y, int w, int h) { return (int)sc(QRT_SYS_WINDOW_PRESENT, win, x, y, w, h); }
int qrt_window_title(int win, const char *t) { return (int)sc(QRT_SYS_WINDOW_TITLE, win, (long)t, 0, 0, 0); }
int qrt_window_close(int win) { return (int)sc(QRT_SYS_WINDOW_CLOSE, win, 0, 0, 0, 0); }
int qrt_wait_event(qrt_event *e, int timeout_ms) { return (int)sc(QRT_SYS_EVENT_WAIT, (long)e, timeout_ms, 0, 0, 0); }
void qrt_keyboard(int show) { sc(QRT_SYS_KEYBOARD, show, 0, 0, 0, 0); }

/* ---- drawing ---------------------------------------------------------------------------- */
static inline uint32_t blend(uint32_t d, uint32_t s, unsigned a) {   /* a: 0..255 */
    uint32_t rb = ((s & 0xff00ff) * a + (d & 0xff00ff) * (255 - a)) >> 8;
    uint32_t g = ((s & 0x00ff00) * a + (d & 0x00ff00) * (255 - a)) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

void qrt_fill(qrt_buffer *b, int x, int y, int w, int h, uint32_t rgb) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > b->w ? b->w : x + w, y1 = y + h > b->h ? b->h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = b->px + (long)yy * b->stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = rgb;
    }
}

static void plot(qrt_buffer *b, int x, int y, uint32_t rgb, unsigned a) {
    if (x < 0 || y < 0 || x >= b->w || y >= b->h || !a) return;
    uint32_t *p = b->px + (long)y * b->stride + x;
    *p = a >= 255 ? rgb : blend(*p, rgb, a);
}

/* coverage of the pixel (x, y) by a disc of radius r at (cx, cy), 0..255 (4x4 samples) */
static unsigned disc_cov(float x, float y, float cx, float cy, float r) {
    unsigned n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            float dx = x + (sx + 0.5f) / 4 - cx, dy = y + (sy + 0.5f) / 4 - cy;
            if (dx * dx + dy * dy <= r * r) n++;
        }
    return n * 255 / 16;
}

void qrt_disc(qrt_buffer *b, int cx, int cy, int r, uint32_t rgb) {
    for (int y = cy - r - 1; y <= cy + r; y++)
        for (int x = cx - r - 1; x <= cx + r; x++) plot(b, x, y, rgb, disc_cov((float)x, (float)y, cx + 0.0f, cy + 0.0f, (float)r));
}

void qrt_round_rect(qrt_buffer *b, int x, int y, int w, int h, int r, uint32_t rgb) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    qrt_fill(b, x + r, y, w - 2 * r, h, rgb);
    qrt_fill(b, x, y + r, r, h - 2 * r, rgb);
    qrt_fill(b, x + w - r, y + r, r, h - 2 * r, rgb);
    int cxs[2] = { x + r, x + w - r }, cys[2] = { y + r, y + h - r };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            int qx0 = i ? cxs[1] : x, qy0 = j ? cys[1] : y;
            for (int yy = qy0; yy < qy0 + r; yy++)
                for (int xx = qx0; xx < qx0 + r; xx++) plot(b, xx, yy, rgb, disc_cov((float)xx, (float)yy, (float)cxs[i], (float)cys[j], (float)r));
        }
}

void qrt_line(qrt_buffer *b, int x0, int y0, int x1, int y1, int width, uint32_t rgb) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int r = width / 2 > 0 ? width / 2 : 1;
    if (!steps) { qrt_disc(b, x0, y0, r, rgb); return; }
    for (int i = 0; i <= steps; i += r > 2 ? r / 2 : 1)
        qrt_disc(b, x0 + dx * i / steps, y0 + dy * i / steps, r, rgb);
    qrt_disc(b, x1, y1, r, rgb);
}

/* ---- text (glyphs from qrt_font.c, generated from Inter at SDK build time) ------------------- */
typedef struct { short adv, x, y, w, h; int off; } qrt_glyph;   /* y: from the top of the line */
extern const qrt_glyph qrt_glyphs[2][95];
extern const unsigned char qrt_glyph_bits[];
extern const int qrt_line_h[2];

int qrt_text_height(int size) { return qrt_line_h[size ? 1 : 0]; }

int qrt_text_width(const char *s, int size) {
    int w = 0;
    for (; *s; s++) { unsigned c = (unsigned char)*s; if (c >= 32 && c < 127) w += qrt_glyphs[size ? 1 : 0][c - 32].adv; }
    return w;
}

int qrt_text(qrt_buffer *b, int x, int y, const char *s, uint32_t rgb, int size) {
    size = size ? 1 : 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c >= 127) { if ((c & 0xc0) == 0x80) continue; c = '?'; }
        const qrt_glyph *g = &qrt_glyphs[size][c - 32];
        const unsigned char *bits = qrt_glyph_bits + g->off;
        for (int yy = 0; yy < g->h; yy++)
            for (int xx = 0; xx < g->w; xx++) plot(b, x + g->x + xx, y + g->y + yy, rgb, bits[yy * g->w + xx]);
        x += g->adv;
    }
    return x;
}

int qrt_event_fd(void) { return (int)sc(QRT_SYS_EVENT_FD, 0, 0, 0, 0, 0); }
