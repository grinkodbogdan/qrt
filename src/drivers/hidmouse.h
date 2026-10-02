/* hidmouse.h - mice and other pointers from USB HID reports.
 * Pure code: builds in the kernel and on the host (tests/test_mouse.c). */
#pragma once
#ifndef HID_HOST_TEST
#include "../kernel/rt.h"
#endif

typedef struct { u16 off, bits; i32 lmin, lmax; u8 present, rel; } hm_field_t;

typedef struct {
    int boot;                    /* boot protocol: buttons, dx, dy (, wheel) */
    int uses_ids;
    u8 report_id;                /* the report that carries X */
    int nbuttons;
    u16 btn_off[8];              /* buttons 1..8 */
    hm_field_t x, y, wheel, pan;
} hidmouse_t;

typedef struct {
    int abs;                     /* 1: x, y are positions 0..65535 (tablets, touch screens as mice) */
    int x, y;                    /* relative: movement in counts */
    int wheel, pan;              /* wheel: + is away from the user */
    u32 buttons;                 /* bit 0 left, 1 right, 2 middle, 3/4 back/forward */
} mouse_report_t;

/* parse a report descriptor; 1 if it describes a pointer (X and Y on the Generic Desktop page) */
int hm_parse(hidmouse_t *m, const u8 *d, int len);
void hm_boot(hidmouse_t *m);     /* use the boot protocol instead */
/* decode one interrupt-IN report; 1 if it was a pointer report */
int hm_decode(const hidmouse_t *m, const u8 *rep, int len, mouse_report_t *out);
