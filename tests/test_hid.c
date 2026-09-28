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

/* Report descriptor read from the tablet by the Touch Lab (touch.txt). */
static const char *wacom_4808_rdesc =
    "05 0d 09 04 a1 01 85 0c 95 01 75 08 26 ff 00 15 00 81 03 09 54 81 02 05 0d 09 22 a1 02 09 42 15"
    "00 25 01 75 01 95 01 81 02 81 03 09 47 81 02 95 05 81 03 75 10 09 51 95 01 81 02 05 01 75 10 95"
    "01 55 0e 65 11 09 30 26 d0 10 35 00 46 34 04 81 02 46 ba 06 09 31 26 e8 1a 81 02 c0 05 0d 09 22"
    "a1 02 09 42 15 00 25 01 75 01 95 01 81 02 81 03 09 47 81 02 95 05 81 03 75 10 09 51 95 01 81 02"
    "05 01 75 10 95 01 55 0e 65 11 09 30 26 d0 10 35 00 46 34 04 81 02 46 ba 06 09 31 26 e8 1a 81 02"
    "c0 05 0d 09 22 a1 02 09 42 15 00 25 01 75 01 95 01 81 02 81 03 09 47 81 02 95 05 81 03 75 10 09"
    "51 95 01 81 02 05 01 75 10 95 01 55 0e 65 11 09 30 26 d0 10 35 00 46 34 04 81 02 46 ba 06 09 31"
    "26 e8 1a 81 02 c0 05 0d 09 22 a1 02 09 42 15 00 25 01 75 01 95 01 81 02 81 03 09 47 81 02 95 05"
    "81 03 75 10 09 51 95 01 81 02 05 01 75 10 95 01 55 0e 65 11 09 30 26 d0 10 35 00 46 34 04 81 02"
    "46 ba 06 09 31 26 e8 1a 81 02 c0 05 0d 09 22 a1 02 09 42 15 00 25 01 75 01 95 01 81 02 81 03 09"
    "47 81 02 95 05 81 03 75 10 09 51 95 01 81 02 05 01 75 10 95 01 55 0e 65 11 09 30 26 d0 10 35 00"
    "46 34 04 81 02 46 ba 06 09 31 26 e8 1a 81 02 c0 05 0d 27 ff ff 00 00 75 10 95 01 09 56 81 02 85"
    "0c 09 55 75 08 95 01 26 ff 00 b1 02 85 0a 06 00 ff 09 c5 96 00 01 b1 02 c0 06 11 ff 09 11 a1 01"
    "85 03 a1 02 09 00 75 08 15 00 26 ff 00 95 27 81 02 c0 85 02 09 00 95 01 b1 02 85 03 09 00 95 3f"
    "b1 02 85 04 09 00 95 0f b1 02 85 07 09 00 96 00 01 b1 02 85 08 09 00 96 87 00 b1 02 85 09 09 00"
    "96 3f 00 b1 02 85 0d 09 00 95 07 b1 02 c0 05 0d 09 0e a1 01 85 0e 09 23 a1 02 09 52 09 53 15 00"
    "25 0a 75 08 95 02 b1 02 c0 c0 05 0d 09 02 a1 01 85 06 09 20 a1 00 09 42 09 44 09 45 09 3c 09 5a"
    "09 32 15 00 25 01 75 01 95 06 81 02 95 02 81 03 05 01 09 30 27 0c 2a 00 00 47 0c 2a 00 00 65 11"
    "55 0d 75 10 95 01 81 02 09 31 27 46 43 00 00 47 46 43 00 00 81 02 45 00 65 00 55 00 05 0d 09 30"
    "26 ff 07 75 10 81 02 06 00 ff 09 5b 75 10 81 02 05 0d 09 5b 75 20 95 01 81 02 06 00 ff 09 00 75"
    "08 95 01 81 02 05 0d 09 3b 75 08 95 01 26 ff 00 15 00 81 02 c0 c0 06 11 ff 09 02 a1 01 85 0b 09"
    "20 a1 00 09 42 09 44 09 45 09 3c 09 5a 09 32 15 00 25 01 75 01 95 06 81 02 95 02 81 03 05 01 09"
    "30 27 0c 2a 00 00 47 0c 2a 00 00 65 11 55 0d 75 10 95 01 81 02 09 31 27 46 43 00 00 47 46 43 00"
    "00 81 02 45 00 65 00 55 00 05 0d 09 30 26 ff 07 75 10 81 02 06 00 ff 09 5b 75 10 81 02 05 0d 09"
    "5b 75 20 95 01 81 02 06 00 ff 09 00 75 08 95 01 81 02 05 0d 09 3b 75 08 95 01 26 ff 00 15 00 81"
    "02 c0 85 05 09 00 95 17 81 02 85 0f 09 00 95 27 81 02 85 0f 09 00 95 07 b1 02 85 11 09 00 95 09"
    "b1 02 85 05 09 00 95 08 b1 02 85 10 09 00 96 3f 00 b1 02 85 0b 09 00 96 3f 00 b1 02 85 12 09 00"
    "75 08 15 00 26 ff 00 96 05 01 b1 02 c0 05 01 09 02 a1 01 85 01 09 01 a1 00 05 09 19 01 29 02 15"
    "00 25 01 95 02 75 01 81 02 95 01 75 06 81 03 05 01 09 30 09 31 26 ff 7f 75 10 95 02 81 02 c0 c0";

static int unhex(const char *s, u8 *out, int cap) {
    int n = 0;
    while (*s && n < cap) {
        while (*s == ' ') s++;
        if (!s[0] || !s[1]) break;
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (u8)v;
        s += 2;
    }
    return n;
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

    /* The real digitizer in a Venue 8 Pro 5855: Wacom 056a:4808 at I2C6/0x0A. */
    static i2chid_t w;
    u8 wd[1024];
    int wl = unhex(wacom_4808_rdesc, wd, sizeof wd);
    CHECK(wl == 928);
    CHECK(hid_parse_report_desc(&w, wd, wl));
    CHECK(w.nfingers == 5 && w.uses_ids && w.touch_report == 12);
    CHECK(w.xmin == 0 && w.xmax == 4304 && w.ymin == 0 && w.ymax == 6888);
    /* finger down: flags 05 = tip + confidence, contact 1, x 2248, y 800 */
    u8 down[40], lift[40];
    CHECK(unhex("0c 00 01 05 01 00 c8 08 20 03" " 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
                " 00 00 00 00 00 00 00 00 00 00 00 00 00 00 24 87", down, sizeof down) == 40);
    CHECK(hid_touch_update(&w, down, 40) == 1 && w.down && w.track_id == 1);
    CHECK(w.x == 2248 * 65535 / 4304 && w.y == 800 * 65535 / 6888);
    /* the exact last report captured on the tablet: flags 04 = lifted */
    CHECK(unhex("0c 00 01 04 01 00 c8 08 20 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
                " 00 00 00 00 00 00 00 00 00 00 00 00 00 00 24 87", lift, sizeof lift) == 40);
    CHECK(hid_touch_update(&w, lift, 40) == 1 && !w.down);

    printf(fails ? "%d failure(s)\n" : "hid parser: all tests passed\n", fails);
    return fails != 0;
}
