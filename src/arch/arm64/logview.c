/*
 * logview.c - the kernel log, full screen, with no touch needed: volume up pressed
 * three times within two seconds shows it (newest lines at the bottom, Linux's boot
 * messages included) and keeps it up to date; three more presses go back to the shell.
 * On a phone whose touch screen is not working yet this is how its log gets read.
 */
#include "arm.h"
#include "../../ui/gfx.h"

static int on;
static u64 presses[3];
static canvas_t cv;

int logview_active(void) { return on; }

/* hal_poll: a volume-up press; 1 if it toggled the view */
int logview_key(void) {
    u64 now = k_now_ms();
    presses[0] = presses[1]; presses[1] = presses[2]; presses[2] = now;
    if (presses[0] && now - presses[0] < 2000) {
        on = !on;
        presses[0] = presses[1] = presses[2] = 0;
        if (!on && cv.px) { canvas_free(&cv); memset(&cv, 0, sizeof cv); }
        return 1;
    }
    return 0;
}

/* draw it (hal_present while active: the shell's frames are not shown) */
void logview_draw(void) {
    static u64 last;
    u64 now = k_now_ms();
    if (now - last < 500) return;
    last = now;
    if (!cv.px) cv = canvas_new((int)k.fb_w, (int)k.fb_h);
    const font_t *f = font_pick(F_MONO, k.fb_w >= 1000 ? 26 : 15);
    int lh = k.fb_w >= 1000 ? 32 : 19, rows = ((int)k.fb_h - lh * 2) / lh;
    gfx_fill(&cv, (rect_t){ 0, 0, (int)k.fb_w, (int)k.fb_h }, RGB(12, 12, 16));
    gfx_text(&cv, f, 8, 4, "QRT kernel log (volume up x3 to close)", RGB(120, 200, 255));
    int n = 0;
    while (klog_line(n)) n++;
    int first = n > rows ? n - rows : 0;
    for (int i = first, y = lh + 8; i < n; i++, y += lh) {
        const char *l = klog_line(i);
        u32 col = !strncmp(l, "linux:", 6) ? RGB(200, 230, 200) : strstr(l, "panic") || strstr(l, "fault") ? RGB(255, 120, 120) : RGB(230, 230, 230);
        gfx_text_fit(&cv, f, 8, y, (int)k.fb_w - 16, l, col);
    }
    fb_present(cv.px, cv.stride, 0, 0, cv.w, cv.h);
}
