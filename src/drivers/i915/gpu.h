/* gpu.h - Intel Gen8 GPU (Cherry Trail HD Graphics): the 3D engine puts the
 * shell's canvas on the panel, rotated on the way, instead of the CPU. */
#pragma once
#include "../pci.h"

int  gpu_probe(pci_dev_t *d);      /* boot: set up the render engine and self-test; 1 = in use */
int  gpu_active(void);             /* present through the GPU */
int  gpu_supported(void);          /* this is a GPU the driver knows */
const char *gpu_status(void);      /* one line for Settings / the device list */
void gpu_set_enabled(int on);      /* Settings switch (remembered in NVRAM) */
int  gpu_enabled(void);
void gpu_stats(u32 *frames, u32 *avg_us, int *coherent);

/* Copy the rectangle (x, y, w, h) of a logical canvas (src: w*h pixels,
 * stride in pixels) to the framebuffer, turned by rot quarter turns as the
 * shell does it.  Returns 0 when the caller must do it on the CPU. */
int  gpu_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h);
