/*
 * logview.c - the kernel log, full screen, with no touch needed: volume up pressed
 * three times within two seconds shows it (newest lines at the bottom, Linux's boot
 * messages included) and keeps it up to date; three more presses go back to the shell.
 * Shown by itself (the last boot's log after a reset, or no touch screen), it goes away
 * after 30 seconds.
 * On a phone whose touch screen is not working yet this is how its log gets read.
 */
#include "arm.h"
#include "../../ui/gfx.h"

static int on, previous;                 /* previous: the last boot's log (plog.c) */
static u64 until;                        /* shown by itself: goes away at this k_now_ms() (0: stays) */
#define AUTO_MS 30000
void logview_show_previous(void) { on = 1; previous = 1; until = k_now_ms() + AUTO_MS; }
void logview_show_current(void) { on = 1; previous = 0; until = k_now_ms() + AUTO_MS; }
static canvas_t cv;
static int back, page = 40;              /* lines scrolled back from the newest; one screen of lines */
/* volume down while the log is up: a screen further back; past the oldest, the newest again */
void logview_older(void) { back += page; }
static void close_view(void) {
    on = 0;
    previous = 0;
    until = 0;
    back = 0;
    if (cv.px) { canvas_free(&cv); memset(&cv, 0, sizeof cv); }
}
/* hal_poll: 1 if a view shown by itself just timed out (the shell redraws) */
int logview_expire(void) {
    if (!on || !until || k_now_ms() < until) return 0;
    close_view();
    return 1;
}
static u64 presses[3];

int logview_active(void) { return on; }

/* hal_poll: a volume-up press; 1 if it toggled the view */
int logview_key(void) {
    u64 now = k_now_ms();
    presses[0] = presses[1]; presses[1] = presses[2]; presses[2] = now;
    if (presses[0] && now - presses[0] < 2000) {
        presses[0] = presses[1] = presses[2] = 0;
        if (on) close_view();
        else { on = 1; previous = 0; until = 0; }               /* opened by hand: stays */
        return 1;
    }
    return 0;
}

/* draw it (hal_present while active: the shell's frames are not shown) */
static void draw(void);
void logview_draw(void) {
    static u64 last;
    u64 now = k_now_ms();
    if (now - last < 500) return;
    last = now;
    draw();
}
void logview_draw_now(void) { draw(); }
static void draw(void) {
    u64 now = k_now_ms();
    if (!cv.px) cv = canvas_new((int)k.fb_w, (int)k.fb_h);
    const font_t *f = font_pick(F_MONO, k.fb_w >= 1000 ? 26 : 15);
    int lh = k.fb_w >= 1000 ? 32 : 19, rows = ((int)k.fb_h - lh * 2) / lh;
    gfx_fill(&cv, (rect_t){ 0, 0, (int)k.fb_w, (int)k.fb_h }, RGB(12, 12, 16));
    char title[128];
    int left = until > now ? (int)((until - now + 999) / 1000) : 0;
    fmt(title, sizeof title, "%s - %s", previous ? "The LAST boot's log (it ended without a shutdown)" : "QRT kernel log",
        until ? "closes by itself" : back ? "vol down: older, vol up x3: close" : "vol down: older, vol up x3: close");
    if (until) { usize l = strlen(title); fmt(title + l, sizeof title - l, " in %d s", left); }
    gfx_text(&cv, f, 8, 4, title, previous ? RGB(255, 200, 120) : RGB(120, 200, 255));
    int n = 0;
    if (previous) n = plog_prev_lines(); else while (klog_line(n)) n++;
    page = rows - 2;
    if (back > 0 && n - rows - back < -page) back = 0;              /* past the oldest: the newest again */
    int first = n - rows - back;
    if (first < 0) first = 0;
    if (back) until = 0;                                            /* being read: it stays */
    for (int i = first, y = lh + 8; i < n; i++, y += lh) {
        const char *l = previous ? plog_prev_line(i) : klog_line(i);
        u32 col = !strncmp(l, "linux:", 6) ? RGB(200, 230, 200) : strstr(l, "panic") || strstr(l, "fault") ? RGB(255, 120, 120) : RGB(230, 230, 230);
        gfx_text_fit(&cv, f, 8, y, (int)k.fb_w - 16, l, col);
    }
    fb_present(cv.px, cv.stride, 0, 0, cv.w, cv.h);
}

/* a panic (main.c): the log, with the panic's lines, drawn once more before everything
 * stops - the screen otherwise keeps whatever it showed before */
void logview_panic(void) {
    on = 1; previous = 0; until = 0; back = 0;
    logview_draw_now();
}
