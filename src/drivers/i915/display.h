/* display.h - external monitors on Cherry Trail's DisplayPort outputs (the
 * Venue 8 Pro 5855's USB-C port in DP Alt Mode), mirroring the screen. */
#pragma once
#include "../../kernel/kernel.h"

void display_start(void);                 /* native mode: start watching the ports (a kernel thread) */
const char *display_status(void);         /* one line for Settings / System Monitor */
int  display_enabled(void);               /* the Settings switch */
void display_set_enabled(int on);
/* the shell drew rectangle d of its canvas: show it on the external monitor too */
void display_mirror(const u32 *px, int w, int h, int stride, int x, int y, int rw, int rh);
int  display_connected(void);             /* a monitor is up and showing our buffer */
int  display_size(int *w, int *h);        /* its mode; returns display_connected() */
const char *display_monitor(void);        /* its name from the EDID */
void display_virtual(int w, int h);       /* tests: plug (w x h) or unplug (0) a monitor that is only memory */
u32  display_checksum(void);              /* tests: a sample of the monitor's picture */
