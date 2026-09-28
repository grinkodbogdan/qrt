/* Host unit test for the HID report-descriptor parser and contact tracker.
 * Build/run: make check */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t i8; typedef int16_t i16; typedef int32_t i32; typedef int64_t i64;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define HID_HOST_TEST
#include "../src/drivers/hidparse.h"
#include "../src/drivers/hidparse.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A typical Windows-style two-finger touchscreen, 12-bit coordinates. */
#define FINGER \
    0x05,0x0d, 0x09,0x22, 0xa1,0x02, \
      0x05,0x0d, 0x09,0x42, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x01, 0x81,0x02, \
      0x95,0x07, 0x81,0x03, \
      0x75,0x08, 0x09,0x51, 0x25,0x7f, 0x95,0x01, 0x81,0x02, \
      0x05,0x01, 0x26,0xff,0x0f, 0x75,0x10, 0x09,0x30, 0x09,0x31, 0x95,0x02, 0x81,0x02, \
    0xc0
static const u8 desc[] = {
    0x05,0x0d, 0x09,0x04, 0xa1,0x01, 0x85,0x01,
    FINGER, FINGER,
    0x05,0x0d, 0x09,0x54, 0x25,0x0a, 0x75,0x08, 0x95,0x01, 0x81,0x02,
    0xc0,
    /* a second application (feature-only max-count report) that must be ignored */
    0x05,0x0d, 0x09,0x0e, 0xa1,0x01, 0x85,0x02, 0x09,0x55, 0x25,0x0a, 0x75,0x08, 0x95,0x01, 0xb1,0x02, 0xc0,
};

static void rep(u8 *r, int tip0, int id0, int x0, int y0, int tip1, int id1, int x1, int y1, int count) {
    r[0] = 1;
    r[1] = tip0; r[2] = id0; r[3] = x0; r[4] = x0 >> 8; r[5] = y0; r[6] = y0 >> 8;
    r[7] = tip1; r[8] = id1; r[9] = x1; r[10] = x1 >> 8; r[11] = y1; r[12] = y1 >> 8;
    r[13] = count;
}

int main(void) {
    static i2chid_t h;
    u8 r[14];
    CHECK(hid_parse_report_desc(&h, desc, sizeof desc));
    CHECK(h.nfingers == 2);
    CHECK(h.uses_ids && h.touch_report == 1);
    CHECK(h.xmin == 0 && h.xmax == 4095 && h.ymax == 4095);

    rep(r, 1, 5, 4095, 0, 0, 0, 0, 0, 1);
    CHECK(hid_touch_update(&h, r, 14) == 1 && h.down && h.x == 65535 && h.y == 0);
    rep(r, 1, 5, 2048, 1024, 1, 6, 10, 10, 2);           /* second finger lands: primary moves */
    CHECK(hid_touch_update(&h, r, 14) == 1 && h.down && h.x == 2048 * 65535 / 4095 && h.y == 1024 * 65535 / 4095);
    rep(r, 1, 6, 30, 30, 0, 0, 0, 0, 1);                  /* hybrid-style report about finger 6 only */
    CHECK(hid_touch_update(&h, r, 14) == 0 && h.down);
    rep(r, 0, 5, 2048, 1024, 1, 6, 30, 30, 2);            /* primary lifts */
    CHECK(hid_touch_update(&h, r, 14) == 1 && !h.down);
    rep(r, 1, 6, 40, 40, 0, 0, 0, 0, 1);                  /* remaining finger becomes primary */
    CHECK(hid_touch_update(&h, r, 14) == 1 && h.down && h.track_id == 6);
    u8 other[2] = { 2, 10 };
    CHECK(hid_touch_update(&h, other, 2) == 0);           /* wrong report id ignored */

    printf(fails ? "%d failure(s)\n" : "hid parser: all tests passed\n", fails);
    return fails != 0;
}
