/* Host test for src/drivers/ish.c (make check): a simulated Cherry Trail
 * Integrated Sensor Hub.
 *
 * The hub answers the way Linux's intel-ish-hid expects firmware to: a reset
 * notice after "host ready" + wake, RESET_NOTIFY_ACK -> ILUP and ISHTP ready in
 * its status, then the bus (HBM) - start, the list of clients (a system client
 * and the HID client), their properties (protocol GUIDs), connect - with flow
 * control credits both ways and messages over 124 bytes in fragments.  Its HID
 * client lists one device whose report descriptor has an accelerometer the
 * way the HID sensor usage tables lay it out: reporting state and power state as
 * selector arrays in logical collections, a report interval, X/Y/Z inputs.
 * The driver must switch the sensor on (power D0, all events, 100 ms), and
 * gravity in the input reports must turn into orientations.
 * Build/run: make check */
#define ISH_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#include "../src/drivers/ish.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

kernel_t k;
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
void klog(const char *f, ...) { va_list ap; va_start(ap, f); if (getenv("VERBOSE")) { vprintf(f, ap); printf("\n"); } va_end(ap); }
static u64 now;
u64 ish_now_ms(void) { return now; }
void thread_sleep_ms(u64 ms) { now += ms; }
void *thread_create(const char *n, void (*fn)(void *), void *a, u64 c) { (void)n; (void)fn; (void)a; (void)c; return NULL; }
void thread_exit(void) { exit(1); }
static u8 regs[0x1000];
u32 pci_read32(u8 b, u8 d, u8 f, u16 off) { (void)b; (void)f; if (d == 0x0a && off == 0) return 0x22d88086; if (d == 0x0a && off == 8) return 0x30; return 0; }
void pci_write32(u8 b, u8 d, u8 f, u16 off, u32 v) { (void)b; (void)d; (void)f; (void)off; (void)v; }
u64 pci_bar(u8 b, u8 d, u8 f, int bar) { (void)b; (void)d; (void)f; (void)bar; return 0x91a00000; }
void *mm_map_mmio(u64 base, u64 size) { (void)size; return base == 0x91a00000 ? regs : NULL; }

/* ---- the hub ------------------------------------------------------------------------------- */
static const u8 rdesc[] = {
    0x05, 0x20, 0x09, 0x01, 0xa1, 0x01,                 /* Sensor page, Sensor, Collection (Application) */
    0x85, 0x01, 0x05, 0x20, 0x09, 0x73, 0xa1, 0x00,     /* Report ID 1, Accelerometer 3D, Collection (Physical) */
    0x0a, 0x16, 0x03, 0x15, 0x00, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0xa1, 0x02,   /* Reporting State: logical */
    0x0a, 0x40, 0x08, 0x0a, 0x41, 0x08, 0x0a, 0x42, 0x08, 0x0a, 0x43, 0x08, 0x0a, 0x44, 0x08, 0x0a, 0x45, 0x08,
    0xb1, 0x00, 0xc0,                                   /* Feature (Data, Array), End Collection */
    0x0a, 0x19, 0x03, 0x15, 0x01, 0x25, 0x06, 0x75, 0x08, 0x95, 0x01, 0xa1, 0x02,   /* Power State: logical, 1-based */
    0x0a, 0x50, 0x08, 0x0a, 0x51, 0x08, 0x0a, 0x52, 0x08, 0x0a, 0x53, 0x08, 0x0a, 0x54, 0x08, 0x0a, 0x55, 0x08,
    0xb1, 0x00, 0xc0,
    0x0a, 0x0e, 0x03, 0x15, 0x00, 0x27, 0xff, 0xff, 0xff, 0x7f, 0x75, 0x20, 0x95, 0x01, 0x55, 0x00, 0xb1, 0x02,   /* Report Interval (ms) */
    0x0a, 0x53, 0x04, 0x17, 0x01, 0x80, 0xff, 0xff, 0x27, 0xff, 0x7f, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x55, 0x0e, 0x81, 0x02,   /* X, 10^-2 g */
    0x0a, 0x54, 0x04, 0x81, 0x02,                       /* Y */
    0x0a, 0x55, 0x04, 0x81, 0x02,                       /* Z */
    0xc0, 0xc0,
};
static u8 feature[7] = { 1, 0, 4, 0, 0, 0, 0 };          /* report 1: no events, power D3 (index 3 + 1), interval 0 */

/* messages to the host, delivered one at a time when the doorbell is free */
static struct { u32 drbl; u8 m[128]; int len; } q[256];
static int qh, qt;
static int host_credits;                                 /* messages the HID client may send to the host */
static int fw_seen_ack, hub_resets;
static void deliver(void) {
    if ((*(u32 *)(regs + I2H_DRBL) & BUSY) || qh == qt) return;
    memcpy(regs + I2H_MSG, q[qh].m, 128);
    *(u32 *)(regs + I2H_DRBL) = q[qh].drbl;
    qh++;
}
static void push(int proto, int mng, const void *p, int len) {
    q[qt].drbl = BUSY | (u32)proto << 10 | (u32)mng << 16 | (u32)len;
    memset(q[qt].m, 0, 128);
    memcpy(q[qt].m, p, (usize)len);
    q[qt].len = len;
    qt++;
    deliver();
}
static void push_ishtp(u8 fw, u8 host, const u8 *p, int len) {
    int off = 0;
    do {
        int n = len - off < 124 ? len - off : 124;
        u8 m[128];
        u32 h = fw | (u32)host << 8 | (u32)n << 16 | (off + n == len ? 1u << 31 : 0);
        memcpy(m, &h, 4); memcpy(m + 4, p + off, (usize)n);
        push(PROTO_ISHTP, 0, m, n + 4);
        off += n;
    } while (off < len);
}
static void credit_host(void) { u8 fc[8] = { 0x08, 5, 1 }; push_ishtp(0, 0, fc, 8); }
static int client_send(const u8 *hdr6, const u8 *payload, int n) {      /* HID client -> host, needs a credit */
    if (host_credits <= 0) { printf("FAIL: the hub had no credit to answer\n"); fails++; return -1; }
    host_credits--;
    u8 m[1024];
    memcpy(m, hdr6, 6); memcpy(m + 6, payload, (usize)n);
    push_ishtp(5, 1, m, n + 6);
    return 0;
}

static u8 cl_buf[1024]; static int cl_len;
static int feature_sets;
static void on_client(const u8 *m, int len) {
    u8 cmd = m[0], dev = m[1];
    u8 h[6] = { (u8)(cmd | 0x80), dev, 0, 0, 0, 0 };
    switch (cmd) {
    case 33: { u8 p[10] = { 1, 7, 0, 0, 0, 0x01, 0xd8, 0x22, 0x86, 0x80 }; client_send(h, p, 10); break; }   /* one device, id 7 */
    case 1: CHECK(dev == 7, "report descriptor of device %u", dev); client_send(h, rdesc, sizeof rdesc); break;
    case 2: CHECK(len >= 7 && m[6] == 1, "GET_FEATURE of report %u", len >= 7 ? m[6] : 0); client_send(h, feature, sizeof feature); break;
    case 3: CHECK(len >= 6 + 7 && m[6] == 1, "SET_FEATURE: %d bytes", len); if (len >= 13) memcpy(feature, m + 6, 7); feature_sets++; client_send(h, NULL, 0); break;
    default: CHECK(0, "unexpected HID command %u", cmd);
    }
}

static void fake_ish_write(u32 o, u32 v) {
    if (o == I2H_DRBL && v == 0) { deliver(); return; }
    if (o != H2I_DRBL || !(v & BUSY)) return;
    int len = (int)(v & 0x3ff), proto = (int)(v >> 10) & 0xf, mng = (int)(v >> 16) & 0xf;
    u8 m[128];
    memcpy(m, regs + H2I_MSG, 128);
    *(u32 *)(regs + H2I_DRBL) = 0;                      /* consumed */
    if (!len && proto == 0) { u32 id = 0x1234; hub_resets++; push(PROTO_MNG, MNG_RESET_NOTIFY, &id, 4); return; }   /* the wake */
    if (proto == PROTO_MNG && mng == MNG_RESET_NOTIFY_ACK) { CHECK((*(u32 *)m & 0xffff) == 0x1234, "reset id %x", *(u32 *)m); fw_seen_ack = 1; *(u32 *)(regs + FWSTS) = 3; return; }
    if (proto != PROTO_ISHTP) return;
    u32 hh; memcpy(&hh, m, 4);
    int fw = (int)(hh & 0xff), host = (int)(hh >> 8) & 0xff, n = (int)(hh >> 16) & 0x1ff, done = (int)(hh >> 31);
    const u8 *d = m + 4;
    if (!fw && !host) {
        switch (d[0]) {
        case 0x01: { u8 r[4] = { 0x81, 1, 0, 1 }; push_ishtp(0, 0, r, 4); break; }
        case 0x04: { u8 r[36] = { 0x84 }; r[4] = 1 << 3 | 1 << 5; push_ishtp(0, 0, r, 36); break; }   /* clients 3 and 5 */
        case 0x05: {
            u8 r[36] = { 0x85, d[1], 0 };
            static const u8 sys[16] = { 0xbe, 0x0a, 0xfb, 0x8b, 0x37, 0x56, 0x4c, 0x4f, 0x9b, 0x45, 0x14, 0x2b, 0x1c, 0x2c, 0x8f, 0x51 };
            memcpy(r + 4, d[1] == 5 ? hid_guid : sys, 16);
            push_ishtp(0, 0, r, 36);
            break;
        }
        case 0x06: { CHECK(d[1] == 5 && d[2] == 1, "connect %u/%u", d[1], d[2]); u8 r[4] = { 0x86, d[1], d[2], 0 }; push_ishtp(0, 0, r, 4); credit_host(); break; }
        case 0x08: CHECK(d[1] == 5 && d[2] == 1, "flow control %u/%u", d[1], d[2]); host_credits++; break;
        default: CHECK(0, "unexpected HBM command %02x", d[0]);
        }
        return;
    }
    CHECK(fw == 5 && host == 1, "message to client %d from %d", fw, host);
    memcpy(cl_buf + cl_len, d, (usize)n); cl_len += n;
    if (done) { int l = cl_len; cl_len = 0; on_client(cl_buf, l); credit_host(); }
}

static void publish(i16 x, i16 y, i16 z) {
    u8 h[6] = { 5, 7, 0, 0, 7, 0 };
    u8 r[7] = { 1, (u8)x, (u8)(x >> 8), (u8)y, (u8)(y >> 8), (u8)z, (u8)(z >> 8) };
    client_send(h, r, 7);
    sensor_step();
}

int main(void) {
    k.is_venue = k.native = 1;
    int r = bringup();
    CHECK(r == 0, "bring-up: %s", status);
    CHECK(hub_resets == 1 && fw_seen_ack, "reset handshake: %d wakes, ack %d", hub_resets, fw_seen_ack);
    CHECK(T.fw_hid == 5, "HID client at %d", T.fw_hid);
    CHECK(nhubs == 1 && hubs[0].dev == 7, "%d hub devices", nhubs);
    /* the accelerometer is on: all events (selector 1 + 0), D0 (selector 1 + 1), 100 ms */
    CHECK(feature_sets == 1 && feature[1] == 1 && feature[2] == 2 && (feature[3] | feature[4] << 8) == 100,
          "feature report after SET: %d sets, %02x %02x %02x %02x", feature_sets, feature[1], feature[2], feature[3], feature[4]);
    CHECK(!strcmp(ish_status(), "accelerometer"), "status: %s", ish_status());

    /* gravity -> orientation, after 600 ms of the same reading */
    for (int i = 0; i < 8; i++) { now += 100; publish(0, -98, 5); }
    CHECK(ish_orientation() == 0, "upright: %d (accel %d %d %d)", ish_orientation(), g[0], g[1], g[2]);
    for (int i = 0; i < 4; i++) { now += 100; publish(-97, 3, 8); }
    CHECK(ish_orientation() == 0, "turned for 400 ms only: %d", ish_orientation());
    for (int i = 0; i < 4; i++) { now += 100; publish(-97, 3, 8); }
    CHECK(ish_orientation() == 1, "left edge down: %d", ish_orientation());
    for (int i = 0; i < 8; i++) { now += 100; publish(96, -4, 2); }
    CHECK(ish_orientation() == 3, "right edge down: %d", ish_orientation());
    for (int i = 0; i < 8; i++) { now += 100; publish(2, 99, 1); }
    CHECK(ish_orientation() == 2, "upside down: %d", ish_orientation());
    for (int i = 0; i < 8; i++) { now += 100; publish(5, -6, -98); }
    CHECK(ish_orientation() == 2, "lying flat keeps the last: %d", ish_orientation());
    CHECK(samples == 40, "%u samples", samples);

    if (fails) { printf("test_ish: %d failures\n", fails); return 1; }
    printf("test_ish: reset handshake, ISHTP bus and credits, HID sensor descriptor, accelerometer on, orientation pass\n");
    return 0;
}
