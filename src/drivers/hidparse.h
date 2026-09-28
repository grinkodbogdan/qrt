/* hidparse.h - HID report descriptor parsing and touch-contact tracking.
 * Pure code: builds in the kernel and on the host (tests/test_hid.c). */
#pragma once
#ifndef HID_HOST_TEST
#include "../kernel/rt.h"
#endif

struct dwi2c;
typedef struct {
    u16 desc_len, bcd, rdesc_len, rdesc_reg, in_reg, in_max, out_reg, out_max,
        cmd_reg, data_reg, vid, pid, ver;
} hid_desc_t;

#define HID_USAGE(page, id) (((u32)(page) << 16) | (id))
#define U_X        HID_USAGE(0x01, 0x30)
#define U_Y        HID_USAGE(0x01, 0x31)
#define U_TIP      HID_USAGE(0x0d, 0x42)
#define U_CONTACT  HID_USAGE(0x0d, 0x51)
#define U_CCOUNT   HID_USAGE(0x0d, 0x54)

typedef struct {
    u32 usage;
    u8 report_id;
    u16 bit_off, bits;
    i32 lmin, lmax;
    i16 finger;          /* index of the enclosing Finger collection, -1 if none */
} hid_field_t;

#define HID_MAX_FIELDS 192

typedef struct {
    struct dwi2c *bus;
    u8 addr;
    u16 desc_reg;
    hid_desc_t d;
    u8 *rdesc;
    hid_field_t f[HID_MAX_FIELDS];
    int nf, nfingers, uses_ids;
    u8 touch_report;     /* report ID carrying finger data */
    i32 xmin, xmax, ymin, ymax;
    /* primary-contact tracking */
    int tracking, track_id, down;
    int x, y;            /* 0..65535 */
    u32 reports, empty_reads, errors;
} i2chid_t;

int  hid_parse_report_desc(i2chid_t *h, const u8 *d, int len);
/* Feed a report (without the 2-byte length) to the contact tracker.
 * Returns 1 if the primary contact changed (h->down, h->x, h->y). */
int  hid_touch_update(i2chid_t *h, const u8 *rep, int len);
