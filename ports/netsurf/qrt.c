/*
 * qrt.c - a libnsfb surface on a QRT window (ports/netsurf.sh copies it into libnsfb's
 * src/surface/).  The window's buffer is the frame buffer (32-bit 0x00RRGGBB), a redrawn
 * box is presented to the shell, and the shell's events become libnsfb's: a touch is the
 * pointer moving there and the first button, the wheel buttons 4 and 5, keys their
 * characters, a resize maps the buffer again, closing the window quits.
 *
 * Licensed under the MIT License (as libnsfb).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <qrt.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"

static int win = -1;
static qrt_buffer buf;
static nsfb_event_t queue[8];                 /* one shell event can be two of libnsfb's */
static int qhead, qn;

static void push(nsfb_event_t e) { if (qn < 8) queue[(qhead + qn++) % 8] = e; }

static int qrt_defaults(nsfb_t *nsfb)
{
    if (win < 0) {
        win = qrt_window_create("NetSurf");
        if (win < 0)
            return -1;
        qrt_window_buffer(win, &buf);
    }
    nsfb->width = buf.w;
    nsfb->height = buf.h;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int qrt_initialise(nsfb_t *nsfb)
{
    nsfb->ptr = (uint8_t *)buf.px;
    nsfb->width = buf.w;
    nsfb->height = buf.h;
    nsfb->linelen = buf.stride * 4;
    return 0;
}

/* the window has the shell's size: a requested size is not ours to choose */
static int qrt_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    (void)width; (void)height; (void)format;
    nsfb->width = buf.w;
    nsfb->height = buf.h;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    nsfb->ptr = (uint8_t *)buf.px;
    nsfb->linelen = buf.stride * 4;
    return 0;
}

static int qrt_finalise(nsfb_t *nsfb)
{
    (void)nsfb;
    if (win >= 0)
        qrt_window_close(win);
    win = -1;
    return 0;
}

static int qrt_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb;
    if (box->x1 > box->x0 && box->y1 > box->y0)
        qrt_window_present(win, box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0);
    return 0;
}

static int key_of(const qrt_event *e)
{
    switch (e->scan) {
    case QRT_KEY_UP: return NSFB_KEY_UP;
    case QRT_KEY_DOWN: return NSFB_KEY_DOWN;
    case QRT_KEY_LEFT: return NSFB_KEY_LEFT;
    case QRT_KEY_RIGHT: return NSFB_KEY_RIGHT;
    case QRT_KEY_HOME: return NSFB_KEY_HOME;
    case QRT_KEY_END: return NSFB_KEY_END;
    case QRT_KEY_PGUP: return NSFB_KEY_PAGEUP;
    case QRT_KEY_PGDN: return NSFB_KEY_PAGEDOWN;
    case QRT_KEY_ESC: return NSFB_KEY_ESCAPE;
    }
    if (e->ch == '\n')
        return NSFB_KEY_RETURN;
    if (e->ch == 127)
        return NSFB_KEY_BACKSPACE;
    return e->ch < 256 ? (int)e->ch : NSFB_KEY_UNKNOWN;
}

static bool qrt_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    if (!qn) {
        qrt_event e;
        int r = qrt_wait_event(&e, timeout < 0 ? -1 : timeout);
        if (r <= 0) {
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_TIMEOUT;
            return true;
        }
        nsfb_event_t m = { .type = NSFB_EVENT_MOVE_ABSOLUTE };
        m.value.vector.x = e.x; m.value.vector.y = e.y; m.value.vector.z = 0;
        nsfb_event_t k = { .type = NSFB_EVENT_KEY_DOWN };
        switch (e.type) {
        case QRT_EV_DOWN:
            push(m);
            k.value.keycode = NSFB_KEY_MOUSE_1; push(k);
            break;
        case QRT_EV_MOVE:
            push(m);
            break;
        case QRT_EV_UP:
            push(m);
            k.type = NSFB_EVENT_KEY_UP; k.value.keycode = NSFB_KEY_MOUSE_1; push(k);
            break;
        case QRT_EV_SCROLL:
            k.value.keycode = e.y > 0 ? NSFB_KEY_MOUSE_5 : NSFB_KEY_MOUSE_4; push(k);
            k.type = NSFB_EVENT_KEY_UP; push(k);
            break;
        case QRT_EV_KEY:
            k.value.keycode = key_of(&e);
            if (k.value.keycode == NSFB_KEY_UNKNOWN)
                break;
            push(k);
            k.type = NSFB_EVENT_KEY_UP; push(k);
            break;
        case QRT_EV_RESIZE:
        case QRT_EV_SHOWN:
            qrt_window_buffer(win, &buf);
            nsfb->ptr = (uint8_t *)buf.px;
            nsfb->linelen = buf.stride * 4;
            if (buf.w != nsfb->width || buf.h != nsfb->height || e.type == QRT_EV_SHOWN) {
                nsfb_event_t rz = { .type = NSFB_EVENT_RESIZE };
                rz.value.resize.w = buf.w; rz.value.resize.h = buf.h;
                nsfb->width = buf.w; nsfb->height = buf.h;
                push(rz);
            }
            break;
        case QRT_EV_CLOSE: {
            nsfb_event_t q = { .type = NSFB_EVENT_CONTROL };
            q.value.controlcode = NSFB_CONTROL_QUIT;
            push(q);
            break;
        }
        }
        if (!qn) {
            event->type = NSFB_EVENT_NONE;
            return true;
        }
    }
    *event = queue[qhead];
    qhead = (qhead + 1) % 8;
    qn--;
    return true;
}

/* touch: no pointer to draw */
static int qrt_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor) { (void)nsfb; (void)cursor; return true; }

const nsfb_surface_rtns_t qrt_rtns = {
    .defaults = qrt_defaults,
    .initialise = qrt_initialise,
    .finalise = qrt_finalise,
    .input = qrt_input,
    .geometry = qrt_geometry,
    .update = qrt_update,
    .cursor = qrt_cursor,
};

NSFB_SURFACE_DEF(qrt, NSFB_SURFACE_LINUX, &qrt_rtns)
