/* hello-window.c - a native QRT program with a window: it fills the app area of the
 * shell, draws with libqrt, follows resizes (rotation, desk mode, the keyboard),
 * takes touches, the mouse and keys, and ends when the window is closed.
 *
 *   qrt-cc -O2 -o hello-window hello-window.c -lqrt */
#include <qrt.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#define BG     0x242424
#define CARD   0x303030
#define TEXT   0xffffff
#define TEXT2  0xb0b0b0
#define ACCENT 0x3584e4

static qrt_buffer b;
static int win, taps, keys_open;
static char typed[64], last[96] = "Touch, click or type";
static int btn_x, btn_y, btn_w = 180, btn_h = 44, kb_x;

static void draw_all(void) {
    qrt_fill(&b, 0, 0, b.w, b.h, BG);
    int m = 32, y = 28;
    qrt_text(&b, m, y, "Hello from a native QRT program", TEXT, QRT_TEXT_TITLE);
    y += qrt_text_height(QRT_TEXT_TITLE) + 6;
    struct utsname u;
    uname(&u);
    char info[160];
    snprintf(info, sizeof info, "%s %s on %s - built with the QRT SDK (musl + libqrt), window %dx%d", u.sysname, u.release, u.machine, b.w, b.h);
    qrt_text(&b, m, y, info, TEXT2, QRT_TEXT_BODY);
    y += 40;
    qrt_round_rect(&b, m, y, b.w - 2 * m, 120, 12, CARD);
    char line[128];
    snprintf(line, sizeof line, "Taps on the button: %d", taps);
    qrt_text(&b, m + 18, y + 18, line, TEXT, QRT_TEXT_BODY);
    qrt_text(&b, m + 18, y + 48, last, TEXT2, QRT_TEXT_BODY);
    snprintf(line, sizeof line, "Typed: %s_", typed);
    qrt_text(&b, m + 18, y + 78, line, TEXT, QRT_TEXT_BODY);
    y += 150;
    btn_x = m; btn_y = y;
    qrt_round_rect(&b, btn_x, btn_y, btn_w, btn_h, 8, ACCENT);
    qrt_text(&b, btn_x + (btn_w - qrt_text_width("Tap me", 0)) / 2, btn_y + 12, "Tap me", TEXT, QRT_TEXT_BODY);
    kb_x = m + btn_w + 16;
    qrt_round_rect(&b, kb_x, btn_y, btn_w, btn_h, 8, 0x4a4a4a);
    const char *kl = keys_open ? "Hide keyboard" : "Keyboard";
    qrt_text(&b, kb_x + (btn_w - qrt_text_width(kl, 0)) / 2, btn_y + 12, kl, TEXT, QRT_TEXT_BODY);
    qrt_text(&b, m, btn_y + btn_h + 24, "Draw below with a finger or the mouse:", TEXT2, QRT_TEXT_BODY);
    qrt_window_present(win, 0, 0, 0, 0);
}

static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

int main(void) {
    win = qrt_window_create("Hello window");
    if (win < 0 || qrt_window_buffer(win, &b)) { printf("hello-window: no window (%d)\n", win); return 1; }
    printf("hello-window: window %d, %dx%d\n", win, b.w, b.h);
    draw_all();
    int down = 0, lx = 0, ly = 0;
    qrt_event e;
    while (qrt_wait_event(&e, -1) >= 0) {
        switch (e.type) {
        case QRT_EV_RESIZE:
            qrt_window_buffer(win, &b);
            draw_all();
            break;
        case QRT_EV_CLOSE:
            printf("hello-window: closed after %d taps\n", taps);
            qrt_window_close(win);
            return 0;
        case QRT_EV_DOWN:
            if (in(e.x, e.y, btn_x, btn_y, btn_w, btn_h)) { taps++; snprintf(last, sizeof last, "Button tapped (%s)", e.scan ? "mouse" : "touch"); draw_all(); break; }
            if (in(e.x, e.y, kb_x, btn_y, btn_w, btn_h)) { keys_open = !keys_open; qrt_keyboard(keys_open); draw_all(); break; }
            down = e.y > btn_y + btn_h + 50;
            lx = e.x; ly = e.y;
            snprintf(last, sizeof last, "%s at %d, %d", e.scan ? "Click" : "Touch", e.x, e.y);
            draw_all();
            break;
        case QRT_EV_MOVE:
            if (down) {
                qrt_line(&b, lx, ly, e.x, e.y, 6, ACCENT);
                int x0 = lx < e.x ? lx : e.x, y0 = ly < e.y ? ly : e.y;
                qrt_window_present(win, x0 - 6, y0 - 6, (lx > e.x ? lx - e.x : e.x - lx) + 12, (ly > e.y ? ly - e.y : e.y - ly) + 12);
                lx = e.x; ly = e.y;
            }
            break;
        case QRT_EV_UP: down = 0; break;
        case QRT_EV_KEY: {
            size_t n = strlen(typed);
            if (e.ch == 8) { if (n) typed[n - 1] = 0; }
            else if (e.ch >= 32 && e.ch < 127 && n + 1 < sizeof typed) { typed[n] = (char)e.ch; typed[n + 1] = 0; }
            draw_all();
            break;
        }
        }
    }
    return 0;
}
