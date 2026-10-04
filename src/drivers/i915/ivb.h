/* ivb.h - Intel Ivy Bridge graphics (the Panasonic FZ-G1): the blitter draws the shell's
 * picture, the display engine mirrors it to HDMI.  gpu.c and display.c call in here. */
#pragma once
#include "../pci.h"

int  ivb_matches(const pci_dev_t *d);
int  ivb_probe(pci_dev_t *d);
int  ivb_present_supported(void);
int  ivb_active(void);
const char *ivb_status(void);
void ivb_set_enabled(int on);
void ivb_stats(u32 *frames, u32 *avg_us);
int  ivb_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h);
void ivb_display_start(void);
const char *ivb_display_status(void);
int  ivb_display_connected(void);
const char *ivb_display_monitor(void);
