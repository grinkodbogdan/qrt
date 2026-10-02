/*
 * clientwin.c - windows of native QRT programs, shown by the shell as apps.
 *
 * A native program (built with the SDK in sdk/) asks for a window with a system
 * call (src/arch/x64/qrtcall.c).  The window is an app like the built-in ones: it
 * has a place in the dock, the overview, the launcher; it fills the app area (on
 * the tablet, or on the external monitor in desk mode) and follows it when that
 * changes size (rotation, the on-screen keyboard, desk mode) - the program gets a
 * resize event and maps the new buffer.  The pixels live in one contiguous piece
 * of kernel memory that the program maps: it draws there, says which rectangle
 * changed (present), and the shell copies that part into the screen.  Touches,
 * mouse clicks and keys go to the program as events.
 *
 * System calls run in the program's threads; the shell runs on its own thread.  The
 * calls only change the window table under a short interrupt lock and leave the
 * rest - adding the app, opening it, new buffers, redraws - to clientwin_poll(),
 * which the shell calls every frame.  draw() may run on any core and only reads.
 */
#include "shell.h"
#include "clientwin.h"

#if defined(__x86_64__)
#include "../arch/x64/lfile.h"

#define MAX_WIN 7
#define QLEN 128

typedef struct {
    int used, ready, id, pid;          /* ready: set up; the shell may show it */
    proc_t *owner;
    char title[48];
    kobj_t *shm;                       /* the current buffer (the window's reference) */
    u32 *px;
    int w, h, stride;
    u64 bytes;
    int added, slot_ok, closing, keyboard;  /* keyboard: 1 show, 2 hide (pending) */
    int dmg_on, dx0, dy0, dx1, dy1;
    qrt_event_t q[QLEN];
    int qh, qn;
    app_t app;
} cwin_t;

static cwin_t win[MAX_WIN];
static int next_id = 1;

#define LOCK   u64 fl_ = irq_save()
#define UNLOCK irq_restore(fl_)

static cwin_t *find(proc_t *p, int id) {
    for (int i = 0; i < MAX_WIN; i++) if (win[i].used && win[i].id == id && win[i].owner == p && !win[i].closing) return &win[i];
    return NULL;
}

static void push(cwin_t *w, qrt_event_t e) {
    LOCK;
    e.window = (u32)w->id;
    if (w->qn < QLEN) w->q[(w->qh + w->qn++) % QLEN] = e;
    else if (e.type == QRT_EV_MOVE) { /* full: drop movement */ }
    else w->q[(w->qh + QLEN - 1) % QLEN] = e;                    /* keep the latest important one */
    if (w->owner && w->owner->qrt_events) eventfd_signal(w->owner->qrt_events);
    UNLOCK;
}

/* a buffer for w x h (the old one goes when the program unmaps it) */
static int new_buffer(cwin_t *w, int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    int stride = width;
    u64 bytes = (u64)stride * (u64)height * 4;
    void *mem;
    kobj_t *o = shm_wrap(bytes, &mem);
    LOCK;
    kobj_t *old = w->shm;
    w->shm = o; w->px = mem; w->w = width; w->h = height; w->stride = stride; w->bytes = bytes;
    UNLOCK;
    if (old) kobj_put(old);
    return 1;
}

/* ---- the app side: one set of functions per slot ------------------------------------------ */
static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    rect_t f = { (int)(cx - r * 0.75f), (int)(cy - r * 0.6f), (int)(r * 1.5f), (int)(r * 1.2f) };
    gfx_rrect_outline(c, f, dp(3), dp(2), fg);
    gfx_fill(c, (rect_t){ f.x, f.y, f.w, (int)(r * 0.3f) }, fg);
}

static void draw_win(cwin_t *w, canvas_t *c, rect_t a) {
    rect_t lim = rect_intersect(c->limit, a);
    if (lim.w <= 0 || lim.h <= 0) return;
    for (int y = lim.y; y < lim.y + lim.h; y++) {
        u32 *dst = c->px + (usize)y * c->stride;
        int wy = y - a.y;
        int x0 = lim.x, x1 = lim.x + lim.w;
        int cw = w->px && wy < w->h ? MIN(x1, a.x + w->w) : x0;        /* columns the buffer covers */
        if (cw > x0) {
            const u32 *src = w->px + (usize)wy * w->stride + (x0 - a.x);
            for (int x = x0; x < cw; x++) dst[x] = 0xff000000u | *src++;
        }
        for (int x = MAX(cw, x0); x < x1; x++) dst[x] = ui.window;      /* not drawn yet (resizing) */
    }
}

static int event_win(cwin_t *w, const event_t *e, rect_t a) {
    qrt_event_t q = { 0 };
    switch (e->type) {
    case EV_DOWN: q.type = QRT_EV_DOWN; break;
    case EV_MOVE: q.type = QRT_EV_MOVE; break;
    case EV_UP: q.type = QRT_EV_UP; break;
    case EV_SCROLL: q.type = QRT_EV_SCROLL; q.y = e->dy; push(w, q); return 0;
    case EV_KEY: q.type = QRT_EV_KEY; q.scan = e->scan; q.ch = e->ch; push(w, q); return 0;
    default: return 0;
    }
    q.x = e->x - a.x; q.y = e->y - a.y;
    q.scan = e->from_mouse ? 1 : 0;                                   /* which kind of pointer */
    push(w, q);
    return 0;
}

static void close_win(cwin_t *w) { if (w->used && !w->closing) push(w, (qrt_event_t){ .type = QRT_EV_CLOSE }); }
static void open_win(cwin_t *w) { if (w->used) push(w, (qrt_event_t){ .type = QRT_EV_SHOWN, .w = w->w, .h = w->h }); }

#define SLOT(k) \
    static void draw_##k(canvas_t *c, rect_t a) { draw_win(&win[k], c, a); } \
    static int event_##k(const event_t *e, rect_t a) { return event_win(&win[k], e, a); } \
    static void close_##k(void) { close_win(&win[k]); } \
    static void open_##k(void) { open_win(&win[k]); }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6)
static void (*const draws[MAX_WIN])(canvas_t *, rect_t) = { draw_0, draw_1, draw_2, draw_3, draw_4, draw_5, draw_6 };
static int (*const events[MAX_WIN])(const event_t *, rect_t) = { event_0, event_1, event_2, event_3, event_4, event_5, event_6 };
static void (*const closes[MAX_WIN])(void) = { close_0, close_1, close_2, close_3, close_4, close_5, close_6 };
static void (*const opens[MAX_WIN])(void) = { open_0, open_1, open_2, open_3, open_4, open_5, open_6 };

/* ---- system calls (program threads) ---------------------------------------------------------- */
const struct app *cw_app_of(int pid) {
    for (int i = 0; i < MAX_WIN; i++)
        if (win[i].used && !win[i].closing && win[i].pid == pid && win[i].slot_ok) return &win[i].app;
    return NULL;
}

int cw_create(proc_t *p, const char *title) {
    LOCK;
    int k = -1;
    for (int i = 0; i < MAX_WIN; i++) if (!win[i].used) { k = i; break; }
    if (k < 0) { UNLOCK; return -24; }                                /* EMFILE */
    cwin_t *w = &win[k];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->id = next_id++;
    w->owner = p;
    w->pid = p->pid;
    UNLOCK;
    strlcpy(w->title, title && title[0] ? title : p->name, sizeof w->title);
    rect_t a = shell_app_area();
    new_buffer(w, a.w, a.h);
    w->app = (app_t){ w->title, "Native QRT program", RGB(0x5e, 0x5c, 0x64), icon, opens[k], draws[k], events[k], NULL, closes[k] };
    { LOCK; w->ready = 1; UNLOCK; }    /* only now may the shell open it: draws[k] is set */
    klog("shell: %s (pid %d) opened window %d, %dx%d", p->name, p->pid, w->id, w->w, w->h);
    return w->id;
}

int cw_buffer(proc_t *p, int id, kobj_t **shm, int *width, int *height, int *stride, u64 *bytes) {
    LOCK;
    cwin_t *w = find(p, id);
    if (!w) { UNLOCK; return -9; }
    kobj_get(w->shm);
    *shm = w->shm; *width = w->w; *height = w->h; *stride = w->stride; *bytes = w->bytes;
    UNLOCK;
    return 0;
}

int cw_present(proc_t *p, int id, int x, int y, int width, int height) {
    LOCK;
    cwin_t *w = find(p, id);
    if (!w) { UNLOCK; return -9; }
    if (width <= 0 || height <= 0) { x = 0; y = 0; width = w->w; height = w->h; }
    int x1 = x + width, y1 = y + height;
    if (!w->dmg_on) { w->dx0 = x; w->dy0 = y; w->dx1 = x1; w->dy1 = y1; w->dmg_on = 1; }
    else { w->dx0 = MIN(w->dx0, x); w->dy0 = MIN(w->dy0, y); w->dx1 = MAX(w->dx1, x1); w->dy1 = MAX(w->dy1, y1); }
    UNLOCK;
    return 0;
}

int cw_close(proc_t *p, int id) {
    LOCK;
    cwin_t *w = find(p, id);
    if (w) w->closing = 1;
    UNLOCK;
    return w ? 0 : -9;
}

int cw_title(proc_t *p, int id, const char *t) {
    cwin_t *w = find(p, id);
    if (!w) return -9;
    strlcpy(w->title, t, sizeof w->title);
    shell_redraw();
    return 0;
}

int cw_next_event(proc_t *p, qrt_event_t *e) {
    LOCK;
    for (int i = 0; i < MAX_WIN; i++) {
        cwin_t *w = &win[i];
        if (!w->used || w->owner != p || !w->qn) continue;
        *e = w->q[w->qh];
        w->qh = (w->qh + 1) % QLEN;
        w->qn--;
        UNLOCK;
        return 1;
    }
    UNLOCK;
    return 0;
}

int cw_has_event(proc_t *p) {
    for (int i = 0; i < MAX_WIN; i++) if (win[i].used && win[i].owner == p && win[i].qn) return 1;
    return 0;
}

void cw_keyboard(proc_t *p, int show) {
    for (int i = 0; i < MAX_WIN; i++) if (win[i].used && win[i].owner == p) win[i].keyboard = show ? 1 : 2;
}

void cw_proc_gone(proc_t *p) {
    LOCK;
    for (int i = 0; i < MAX_WIN; i++) if (win[i].used && win[i].owner == p) win[i].closing = 1;
    UNLOCK;
}

/* ---- the shell thread ------------------------------------------------------------------------ */
void clientwin_poll(void) {
    rect_t a = shell_app_area();
    for (int i = 0; i < MAX_WIN; i++) {
        cwin_t *w = &win[i];
        if (!w->used || !w->ready) continue;
        if (w->closing || w->owner->exited) {
            if (w->added) shell_app_remove(&w->app);
            LOCK;
            kobj_t *o = w->shm;
            w->shm = NULL; w->px = NULL;
            w->used = 0;
            UNLOCK;
            if (o) kobj_put(o);
            continue;
        }
        if (!w->added) {
            w->added = 1;
            if (shell_app_add(&w->app) >= 0) { w->slot_ok = 1; shell_app_open(&w->app); }
            else { klog("shell: no room for %s's window", w->owner->name); w->closing = 1; }
            continue;
        }
        if (w->keyboard) {
            if (shell_app_showing(&w->app)) {
                klog("shell: %s's window %s the keyboard", w->owner->name, w->keyboard == 1 ? "shows" : "hides");
                shell_keyboard(w->keyboard == 1);
            }
            w->keyboard = 0;
        }
        /* the app area changed (rotation, keyboard, desk mode): a new buffer, and the program is told */
        if (shell_app_showing(&w->app) && (a.w != w->w || a.h != w->h) && a.w > 0 && a.h > 0) {
            new_buffer(w, a.w, a.h);
            push(w, (qrt_event_t){ .type = QRT_EV_RESIZE, .w = a.w, .h = a.h });
            shell_damage(a);
        }
        if (w->dmg_on) {
            LOCK;
            rect_t d = { a.x + w->dx0, a.y + w->dy0, w->dx1 - w->dx0, w->dy1 - w->dy0 };
            w->dmg_on = 0;
            UNLOCK;
            if (shell_app_showing(&w->app)) shell_damage(rect_intersect(d, a));
        }
    }
}

#else
void clientwin_poll(void) {}
#endif
