/* clientwin.h - windows of native QRT programs (clientwin.c).  The event layout is
 * the one sdk/libqrt/qrt.h gives programs. */
#pragma once
#include "../kernel/kernel.h"

enum { QRT_EV_DOWN = 1, QRT_EV_MOVE, QRT_EV_UP, QRT_EV_KEY, QRT_EV_SCROLL, QRT_EV_RESIZE, QRT_EV_CLOSE, QRT_EV_SHOWN };
typedef struct { u32 type, window; i32 x, y; u32 scan, ch; i32 w, h; } qrt_event_t;   /* 32 bytes */

#if defined(__x86_64__)
#include "../arch/x64/proc.h"
int  cw_create(proc_t *p, const char *title);
int  cw_buffer(proc_t *p, int id, kobj_t **shm, int *w, int *h, int *stride, u64 *bytes);
int  cw_present(proc_t *p, int id, int x, int y, int w, int h);
int  cw_close(proc_t *p, int id);
int  cw_title(proc_t *p, int id, const char *t);
int  cw_next_event(proc_t *p, qrt_event_t *e);
int  cw_has_event(proc_t *p);
void cw_keyboard(proc_t *p, int show);
void cw_proc_gone(proc_t *p);
struct app;
const struct app *cw_app_of(int pid);     /* the shell app of a window of process pid, or NULL */
#endif
