/* qrt.h - the QRT platform API for native programs (link with -lqrt).
 *
 * A window is an app in the QRT shell: it fills the app area of whichever screen
 * the shell is on (the tablet, or the external monitor in desk mode) and has its
 * place in the dock and the overview.  The program draws into the window's buffer
 * (32-bit 0x00RRGGBB pixels) and presents the rectangle it changed; touches, mouse
 * clicks, scrolling and keys arrive as events.  When the shell changes the window's
 * size it sends QRT_EV_RESIZE: call qrt_window_buffer() again and redraw. */
#ifndef QRT_H
#define QRT_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QRT_EV_DOWN = 1,    /* x, y: a finger or the mouse button went down; scan: 1 for the mouse */
    QRT_EV_MOVE,        /* x, y */
    QRT_EV_UP,          /* x, y */
    QRT_EV_KEY,         /* ch: the character (0 for a key without one), scan: QRT_KEY_* */
    QRT_EV_SCROLL,      /* y: wheel steps, positive towards the end */
    QRT_EV_RESIZE,      /* w, h: the new size - map the buffer again */
    QRT_EV_CLOSE,       /* the user closed the window */
    QRT_EV_SHOWN,       /* the window came on screen */
};
enum { QRT_KEY_UP = 1, QRT_KEY_DOWN, QRT_KEY_RIGHT, QRT_KEY_LEFT, QRT_KEY_HOME, QRT_KEY_END,
       QRT_KEY_PGUP = 9, QRT_KEY_PGDN, QRT_KEY_ESC = 0x17 };

typedef struct { uint32_t type, window; int32_t x, y; uint32_t scan, ch; int32_t w, h; } qrt_event;
typedef struct { uint32_t *px; int w, h, stride; } qrt_buffer;   /* stride in pixels */

int  qrt_window_create(const char *title);                    /* window id, or -errno */
int  qrt_window_buffer(int win, qrt_buffer *b);               /* map its pixels (again after a resize) */
int  qrt_window_present(int win, int x, int y, int w, int h);  /* show a changed rectangle (w = 0: all) */
int  qrt_window_title(int win, const char *title);
int  qrt_window_close(int win);
int  qrt_wait_event(qrt_event *e, int timeout_ms);            /* 1: an event, 0: timed out, -errno; -1 ms waits forever */
void qrt_keyboard(int show);                                  /* the on-screen keyboard */

/* drawing into a buffer (clipped) */
void qrt_fill(qrt_buffer *b, int x, int y, int w, int h, uint32_t rgb);
void qrt_round_rect(qrt_buffer *b, int x, int y, int w, int h, int radius, uint32_t rgb);
void qrt_disc(qrt_buffer *b, int cx, int cy, int r, uint32_t rgb);
void qrt_line(qrt_buffer *b, int x0, int y0, int x1, int y1, int width, uint32_t rgb);
/* text in Inter, the shell's typeface: size QRT_TEXT_BODY (15 px) or QRT_TEXT_TITLE (23 px) */
enum { QRT_TEXT_BODY = 0, QRT_TEXT_TITLE = 1 };
int  qrt_text(qrt_buffer *b, int x, int y, const char *utf8, uint32_t rgb, int size);  /* x after the text; y is the top */
int  qrt_text_width(const char *utf8, int size);
int  qrt_text_height(int size);

#ifdef __cplusplus
}
#endif
#endif
