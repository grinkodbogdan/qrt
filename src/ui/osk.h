/* osk.h - on-screen keyboard (see osk.c). */
#pragma once
#include "shell.h"

int    osk_visible(void);
void   osk_show(void);
void   osk_hide(void);
void   osk_pin(int on);                      /* pinned: osk_hide() has no effect, the hide key still closes it */
int    osk_height(void);                     /* px while visible, else 0 */
rect_t osk_rect(rect_t area);                /* the panel within the content area */
void   osk_draw(canvas_t *c, rect_t area);
int    osk_pointer(const event_t *e, rect_t area, event_t *out, int max);   /* key events produced */
int    osk_tick(u64 now, event_t *out, int max);                            /* backspace repeat */
