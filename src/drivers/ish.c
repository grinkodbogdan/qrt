/*
 * ish.c - the Integrated Sensor Hub of Cherry Trail (PCI 8086:22d8): the
 * accelerometer (auto-rotate) and the ambient light sensor (0.9.5).
 *
 * The sensors sit behind a small microcontroller whose firmware the BIOS loads.
 * Talking to it takes four layers, as in Linux's drivers/hid/intel-ish-hid:
 *
 *  1. IPC: a doorbell register and 128 bytes of message registers each way.
 *     A doorbell is BUSY | protocol (bits 10-13) | length; the receiver clears
 *     it.  Protocol 3 is management (reset handshake), 1 is ISHTP.
 *  2. Bring-up: say "host ready", wake it; the firmware sends RESET_NOTIFY, we
 *     answer RESET_NOTIFY_ACK and wait for ILUP and ISHTP-ready in its status.
 *  3. ISHTP: each message starts with {fw_addr, host_addr, length:9, complete:1};
 *     address 0/0 is the bus itself (HBM): START, ENUM (which clients exist),
 *     PROPERTIES (each client's protocol GUID), CONNECT to the HID client, and
 *     flow control - one credit per message, each way.
 *  4. HID over ISHTP: ENUM_DEVICES, the report descriptor of each sensor hub
 *     device, GET/SET_FEATURE_REPORT to switch a sensor on (power D0, report all
 *     events, an interval), and input reports published as they come.
 *
 * The report descriptor follows the HID sensor usage tables: a physical
 * collection per sensor (Accelerometer 3D 0x200073, Ambient Light 0x200041),
 * properties as feature fields (selector arrays inside logical collections),
 * data as input fields.  Gravity gives the orientation: the shell turns the
 * screen when it is clear for 600 ms (Settings: Auto-rotate).
 *
 * Nothing of this runs in QEMU; tests/test_ish.c drives it against a simulated
 * hub, and every step logs what it found.
 */
#include "ish.h"
#include "pci.h"
#if defined(__x86_64__) && !defined(ISH_HOST_TEST)
#include "../arch/x64/sched.h"
#elif defined(ISH_HOST_TEST)
void thread_sleep_ms(u64 ms);
void *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3);
void thread_exit(void);
void *mm_map_mmio(u64 base, u64 size);
#endif

static char status[160] = "not started";
static volatile int orient = -1;              /* 0..3, -1 unknown */
static volatile int lux = -1;
static volatile i32 g[3];                     /* the last acceleration, in the sensor's units */
static volatile u32 samples;

const char *ish_status(void) { return status; }
int ish_orientation(void) { return orient; }
int ish_lux(void) { return lux; }
void ish_accel(int out[3]) { out[0] = g[0]; out[1] = g[1]; out[2] = g[2]; }

#if defined(__x86_64__) || defined(ISH_HOST_TEST)
/* ---- registers ---------------------------------------------------------------------- */
#define FWSTS      0x34
#define HOST_COMM  0x38
#define H2I_DRBL   0x48
#define I2H_DRBL   0x54
#define I2H_MSG    0x60
#define H2I_MSG    0xe0
#define RMP2       0x368
#define BUSY       (1u << 31)
#define PROTO_ISHTP 1
#define PROTO_MNG   3
#define MNG_RESET_NOTIFY     3
#define MNG_RESET_NOTIFY_ACK 4

static volatile u8 *R;
static u32 rr(u32 o) { return *(volatile u32 *)(R + o); }
#ifdef ISH_HOST_TEST
static void fake_ish_write(u32 o, u32 v);
static void rw(u32 o, u32 v) { *(volatile u32 *)(R + o) = v; fake_ish_write(o, v); }
#else
static void rw(u32 o, u32 v) { *(volatile u32 *)(R + o) = v; }
#endif

/* ---- IPC ------------------------------------------------------------------------- */
static int ipc_send(int proto, int mng, const void *p, int len) {
    for (int i = 0; i < 200 && (rr(H2I_DRBL) & BUSY); i++) thread_sleep_ms(1);
    if (rr(H2I_DRBL) & BUSY) return -1;
    const u8 *b = p;
    for (int i = 0; i < len; i += 4) { u32 w = 0; memcpy(&w, b + i, MIN(4, len - i)); rw(H2I_MSG + (u32)i, w); }
    rw(H2I_DRBL, BUSY | (u32)proto << 10 | (u32)mng << 16 | (u32)len);
    return 0;
}

/* ---- ISHTP state ---------------------------------------------------------------------- */
#define HOST_ADDR 1
static struct {
    int hw_ready, reset_id;
    int started, enum_done, props_done, connect_done, connect_status;
    u8 clients[32];               /* the bitmap of firmware clients */
    u8 props_guid[16];
    int props_addr;
    int fw_hid;                   /* the HID client's firmware address */
    int credits;                  /* how many messages the firmware lets us send */
    u8 rx[4096]; int rx_len;      /* reassembly of a client message */
    u8 msg[4096]; int msg_len, msg_ready;
} T;

static const u8 hid_guid[16] = { 0x58, 0xcd, 0xae, 0x33, 0x79, 0xb6, 0x54, 0x4e, 0x9b, 0xd9, 0xa0, 0x4d, 0x34, 0xf0, 0xc2, 0x26 };

static int ishtp_send(u8 fw, u8 host, const void *p, int len) {
    const u8 *b = p;
    int off = 0;
    do {
        int n = MIN(len - off, 124);
        u8 m[128];
        u32 h = fw | (u32)host << 8 | (u32)n << 16 | (off + n == len ? 1u << 31 : 0);
        memcpy(m, &h, 4);
        memcpy(m + 4, b + off, (usize)n);
        if (ipc_send(PROTO_ISHTP, 0, m, n + 4)) return -1;
        off += n;
    } while (off < len);
    return 0;
}
static int hbm(const void *p, int len) { return ishtp_send(0, 0, p, len); }
static void flow_control(void) { u8 m[8] = { 0x08, (u8)T.fw_hid, HOST_ADDR }; hbm(m, 8); }

static void on_hbm(const u8 *d, int len) {
    switch (d[0]) {
    case 0x81: T.started = 1; break;                                   /* HOST_START_RES */
    case 0x84: if (len >= 36) memcpy(T.clients, d + 4, 32); T.enum_done = 1; break;
    case 0x85: if (len >= 20) { T.props_addr = d[1]; memcpy(T.props_guid, d + 4, 16); } T.props_done = 1; break;
    case 0x86: T.connect_status = len >= 4 ? d[3] : -1; T.connect_done = 1; break;
    case 0x08: if (len >= 3 && d[1] == T.fw_hid && d[2] == HOST_ADDR) T.credits++; break;   /* the firmware's credit */
    default: break;
    }
}

/* one message from the hub, if any */
static int poll_once(void) {
    u32 d = rr(I2H_DRBL);
    if (!(d & BUSY)) return 0;
    int len = (int)(d & 0x3ff), proto = (int)(d >> 10) & 0xf, mng = (int)(d >> 16) & 0xf;
    u8 m[132];
    len = MIN(len, 128);
    for (int i = 0; i < len; i += 4) { u32 w = rr(I2H_MSG + (u32)i); memcpy(m + i, &w, 4); }
    rw(I2H_DRBL, 0);                                                    /* done with it */
    if (proto == PROTO_MNG) {
        if (mng == MNG_RESET_NOTIFY) { u32 id; memcpy(&id, m, 4); T.reset_id = (int)(id & 0xffff) | 0x10000; }
        else if (mng == MNG_RESET_NOTIFY_ACK) T.hw_ready = 1;
    } else if (proto == PROTO_ISHTP && len >= 4) {
        u32 h;
        memcpy(&h, m, 4);
        int fw = (int)(h & 0xff), host = (int)(h >> 8) & 0xff, n = (int)(h >> 16) & 0x1ff, done = (int)(h >> 31);
        n = MIN(n, len - 4);
        if (!fw && !host) on_hbm(m + 4, n);
        else if (fw == T.fw_hid && host == HOST_ADDR) {
            if (T.rx_len + n <= (int)sizeof T.rx) { memcpy(T.rx + T.rx_len, m + 4, (usize)n); T.rx_len += n; }
            if (done) {
                memcpy(T.msg, T.rx, (usize)T.rx_len);
                T.msg_len = T.rx_len; T.msg_ready = 1; T.rx_len = 0;
                flow_control();                                         /* room for the next one */
            }
        }
    }
    return 1;
}

static int wait_for(volatile int *flag, int ms) {
    for (int i = 0; i < ms && !*flag; i++) { while (poll_once()) {} if (!*flag) thread_sleep_ms(1); }
    return *flag ? 0 : -1;
}

/* ---- HID over ISHTP ------------------------------------------------------------------------- */
enum { H_HID_DESC = 0, H_REPORT_DESC = 1, H_GET_FEATURE = 2, H_SET_FEATURE = 3, H_GET_INPUT = 4, H_INPUT = 5, H_INPUT_LIST = 6, H_ENUM = 33 };

static void on_input(int dev, const u8 *rep, int len);

/* send to the HID client (waits for a credit); returns 0 */
static int hid_send(const void *p, int len) {
    for (int i = 0; i < 500 && T.credits <= 0; i++) { while (poll_once()) {} if (T.credits <= 0) thread_sleep_ms(1); }
    if (T.credits <= 0) return -1;
    T.credits--;
    return ishtp_send((u8)T.fw_hid, HOST_ADDR, p, len);
}

/* a request and its answer (command | 0x80); input reports that arrive meanwhile are handled */
static int hid_call(u8 cmd, u8 dev, const u8 *extra, int nextra, u8 *out, int cap) {
    u8 m[256];
    memset(m, 0, 6);
    m[0] = cmd; m[1] = dev;
    if (nextra) memcpy(m + 6, extra, (usize)nextra);
    T.msg_ready = 0;
    if (hid_send(m, 6 + nextra)) return -1;
    for (int t = 0; t < 1000; t++) {
        while (poll_once()) {}
        if (T.msg_ready) {
            T.msg_ready = 0;
            if (T.msg_len >= 6 && T.msg[0] == (cmd | 0x80)) {
                int n = MIN(T.msg_len - 6, cap);
                if (out && n > 0) memcpy(out, T.msg + 6, (usize)n);
                return n;
            }
            if (T.msg_len >= 6 && (T.msg[0] & 0x7f) == H_INPUT) on_input(T.msg[1], T.msg + 6, T.msg_len - 6);
        }
        thread_sleep_ms(1);
    }
    return -1;
}

/* ---- the report descriptor -------------------------------------------------------------- */
typedef struct {
    u8 report_id, type;           /* 1 input, 2 feature */
    u16 bit, size, count;
    i32 lmin;
    i8 exponent;
    u32 sensor;                   /* the enclosing physical collection's usage (the sensor) */
    u32 prop;                     /* the enclosing logical collection's usage (a property), or the field's usage */
    u32 usages[8]; int nusages;   /* the usages listed for it (selectors for arrays) */
} sfield_t;

#define MAX_SF 96
typedef struct { int dev; sfield_t f[MAX_SF]; int nf; } shub_t;
static shub_t hubs[4];
static int nhubs;

static void parse_rdesc(shub_t *h, const u8 *d, int len) {
    u32 page = 0, usages[8], coll[8];
    u8 coll_type[8];
    int nu = 0, depth = 0;
    i32 lmin = 0;
    u32 rsize = 0, rcount = 0;
    i8 exponent = 0;
    u8 rid = 0;
    u16 bits[2][256];
    memset(bits, 0, sizeof bits);
    h->nf = 0;
    for (int i = 0; i < len;) {
        u8 b = d[i++];
        if (b == 0xfe) { if (i + 1 < len) i += 2 + d[i]; continue; }   /* long item */
        int sz = (b & 3) == 3 ? 4 : (b & 3);
        u32 v = 0;
        for (int k = 0; k < sz && i + k < len; k++) v |= (u32)d[i + k] << (8 * k);
        i32 sv = sz == 1 ? (i8)v : sz == 2 ? (i16)v : (i32)v;
        i += sz;
        switch (b & 0xfc) {
        case 0x04: page = v; break;                                    /* usage page */
        case 0x14: lmin = sv; break;
        case 0x54: exponent = (i8)((v & 8) ? (int)v - 16 : (int)v); break;   /* unit exponent, a 4-bit signed value */
        case 0x74: rsize = v; break;
        case 0x84: rid = (u8)v; break;
        case 0x94: rcount = v; break;
        case 0x08: if (nu < 8) usages[nu++] = sz == 4 ? v : (page << 16 | v); break;   /* usage */
        case 0xa0:                                                      /* collection */
            if (depth < 8) { coll[depth] = nu ? usages[0] : 0; coll_type[depth] = (u8)v; depth++; }
            nu = 0;
            break;
        case 0xc0: if (depth) depth--; nu = 0; break;                   /* end collection */
        case 0x80: case 0xb0: {                                         /* input, feature */
            int type = (b & 0xfc) == 0x80 ? 1 : 2;
            u32 sensor = 0, prop = 0;
            for (int c = 0; c < depth; c++) { if (coll_type[c] == 0) sensor = coll[c]; if (coll_type[c] == 2) prop = coll[c]; }
            int arr = !(v & 2);                                         /* Array: one value picks a usage */
            u16 *bitp = &bits[type - 1][rid];
            if (arr || rcount <= 1) {
                if (h->nf < MAX_SF) {
                    sfield_t *f = &h->f[h->nf++];
                    memset(f, 0, sizeof *f);
                    f->report_id = rid; f->type = (u8)type; f->bit = *bitp; f->size = (u16)rsize; f->count = (u16)rcount;
                    f->lmin = lmin; f->exponent = exponent; f->sensor = sensor;
                    f->prop = arr && prop ? prop : nu ? usages[0] : prop;
                    f->nusages = nu; memcpy(f->usages, usages, sizeof usages);
                }
            } else {                                                    /* variables: one field per usage */
                for (u32 k = 0; k < rcount && h->nf < MAX_SF; k++) {
                    sfield_t *f = &h->f[h->nf++];
                    memset(f, 0, sizeof *f);
                    f->report_id = rid; f->type = (u8)type; f->bit = (u16)(*bitp + k * rsize); f->size = (u16)rsize; f->count = 1;
                    f->lmin = lmin; f->exponent = exponent; f->sensor = sensor;
                    f->prop = nu ? usages[MIN((int)k, nu - 1)] : 0;
                }
            }
            *bitp = (u16)(*bitp + rsize * rcount);
            nu = 0;
            break;
        }
        default: break;
        }
    }
}

static sfield_t *field(shub_t *h, u32 sensor, int type, u32 prop) {
    for (int i = 0; i < h->nf; i++) if (h->f[i].sensor == sensor && h->f[i].type == type && h->f[i].prop == prop) return &h->f[i];
    return NULL;
}
static i32 get_bits(const u8 *r, int len, int bit, int size, int sign) {
    u32 v = 0;
    for (int i = 0; i < size; i++) { int b = bit + i; if (b / 8 < len && (r[b / 8] >> (b % 8)) & 1) v |= 1u << i; }
    if (sign && size < 32 && (v >> (size - 1)) & 1) v |= ~0u << size;
    return (i32)v;
}
static void put_bits(u8 *r, int bit, int size, u32 v) {
    for (int i = 0; i < size; i++) { int b = bit + i; if ((v >> i) & 1) r[b / 8] |= (u8)(1 << (b % 8)); else r[b / 8] &= (u8)~(1 << (b % 8)); }
}

/* switch one sensor on: power D0, all events, an interval (read, change, write back) */
static int sensor_on(shub_t *h, u32 sensor, int interval_ms) {
    sfield_t *pw = field(h, sensor, 2, 0x200319), *rs = field(h, sensor, 2, 0x200316), *iv = field(h, sensor, 2, 0x20030e);
    if (!pw && !rs) return -1;
    u8 rid = (pw ? pw : rs)->report_id;
    u8 rep[128];
    memset(rep, 0, sizeof rep);
    int n = hid_call(H_GET_FEATURE, (u8)h->dev, &rid, 1, rep, sizeof rep);
    if (n < 1) { n = 1; rep[0] = rid; }                                /* no answer: build it from zero */
    int off = rid ? 8 : 0, total = n;
    for (int which = 0; which < 3; which++) {
        sfield_t *f = which == 0 ? pw : which == 1 ? rs : iv;
        if (!f || f->report_id != rid) continue;
        u32 want = which == 0 ? 0x200851 : 0x200841, val = (u32)interval_ms;
        if (which < 2) {
            int idx = -1;
            for (int k = 0; k < f->nusages; k++) if (f->usages[k] == want) idx = k;
            if (idx < 0) continue;
            val = (u32)(idx + f->lmin);
        }
        put_bits(rep, off + f->bit, f->size, val);
        total = MAX(total, (off + f->bit + f->size + 7) / 8);
    }
    rep[0] = rid ? rid : rep[0];
    return hid_call(H_SET_FEATURE, (u8)h->dev, rep, total, NULL, 0) < 0 ? -1 : 0;
}

/* ---- input ------------------------------------------------------------------------------- */
static int pending_orient = -1;
static u64 pending_since;
u64 ish_now_ms(void);

static void on_input(int dev, const u8 *rep, int len) {
    shub_t *h = NULL;
    for (int i = 0; i < nhubs; i++) if (hubs[i].dev == dev) h = &hubs[i];
    if (!h || len < 1) return;
    u8 rid = rep[0];
    sfield_t *x = field(h, 0x200073, 1, 0x200453), *y = field(h, 0x200073, 1, 0x200454), *z = field(h, 0x200073, 1, 0x200455);
    if (x && y && x->report_id == rid) {
        int off = rid ? 8 : 0;
        g[0] = get_bits(rep, len, off + x->bit, x->size, 1);
        g[1] = get_bits(rep, len, off + y->bit, y->size, 1);
        g[2] = z && z->report_id == rid ? get_bits(rep, len, off + z->bit, z->size, 1) : 0;
        samples++;
        /* gravity points down; HID sensor axes: x right, y up, z out of the screen in the natural orientation */
        i32 ax = g[0] < 0 ? -g[0] : g[0], ay = g[1] < 0 ? -g[1] : g[1], az = g[2] < 0 ? -g[2] : g[2];
        int o = -1;
        if (ay > 2 * ax && ay > az / 2) o = g[1] < 0 ? 0 : 2;          /* upright, upside down */
        else if (ax > 2 * ay && ax > az / 2) o = g[0] < 0 ? 1 : 3;     /* left edge down, right edge down */
        u64 now = ish_now_ms();
        if (o != pending_orient) { pending_orient = o; pending_since = now; }
        else if (o >= 0 && o != orient && now - pending_since >= 600) orient = o;
    }
    sfield_t *l = field(h, 0x200041, 1, 0x2004d1);
    if (l && l->report_id == rid) lux = get_bits(rep, len, (rid ? 8 : 0) + l->bit, l->size, 0);
}

/* ---- bring-up ------------------------------------------------------------------------------ */
static int bringup(void) {
    u32 id = pci_read32(0, 0x0a, 0, 0);
    if (id != 0x22d88086) { strlcpy(status, "no sensor hub at PCI 00:0a.0", sizeof status); return -1; }
    pci_write32(0, 0x0a, 0, 0x04, pci_read32(0, 0x0a, 0, 0x04) | 0x6);
    u64 bar = pci_bar(0, 0x0a, 0, 0);
    R = bar ? mm_map_mmio(bar, 0x1000) : NULL;
    if (!R) { strlcpy(status, "the sensor hub has no registers mapped", sizeof status); return -1; }
    u8 rev = (u8)pci_read32(0, 0x0a, 0, 0x08);
    u32 fw = rr(FWSTS);
    klog("sensors: hub rev %02x at %llx, firmware status %08x", rev, bar, fw);
    if (fw == ~0u) { strlcpy(status, "the sensor hub does not answer", sizeof status); return -1; }

    /* host ready (Cherry Trail's variants), wake it, and wait for its reset notice */
    rw(HOST_COMM, rr(HOST_COMM) | 0x80);
    if (rev == 0x06 || (rev & 0x70) == 0x00) rw(HOST_COMM, 0x81);
    else rw(HOST_COMM, rr(HOST_COMM) | (1u << 31) | 0x81);
    rw(RMP2, 1);
    rw(H2I_DRBL, BUSY);
    for (int i = 0; i < 3000 && !T.reset_id; i++) { while (poll_once()) {} if (!T.reset_id) thread_sleep_ms(1); }
    if (T.reset_id) {
        for (int i = 0; i < 200 && (rr(H2I_DRBL) & BUSY); i++) thread_sleep_ms(1);
        rw(HOST_COMM, rr(HOST_COMM) | 0x80);
        u32 rid = (u32)T.reset_id & 0xffff;
        ipc_send(PROTO_MNG, MNG_RESET_NOTIFY_ACK, &rid, 4);
    } else {
        /* no notice: start the handshake ourselves */
        u32 m = 1;                                                      /* reset id 1 */
        rw(I2H_DRBL, 0);
        ipc_send(PROTO_MNG, MNG_RESET_NOTIFY, &m, 4);
        wait_for(&T.hw_ready, 2000);
    }
    for (int i = 0; i < 2000 && (rr(FWSTS) & 3) != 3; i++) thread_sleep_ms(1);
    if ((rr(FWSTS) & 3) != 3) { fmt(status, sizeof status, "the sensor hub's firmware did not come up (status %08x)", rr(FWSTS)); return -1; }

    /* the bus: start, the clients, the HID client */
    u8 start[4] = { 0x01, 0, 0, 1 };                                    /* HOST_START_REQ, version 1.0 */
    hbm(start, 4);
    if (wait_for(&T.started, 2000)) { strlcpy(status, "the sensor hub's bus did not start", sizeof status); return -1; }
    u8 en[4] = { 0x04 };
    hbm(en, 4);
    if (wait_for(&T.enum_done, 2000)) { strlcpy(status, "the sensor hub did not list its clients", sizeof status); return -1; }
    T.fw_hid = 0;
    for (int a = 1; a < 256 && !T.fw_hid; a++) {
        if (!(T.clients[a / 8] >> (a % 8) & 1)) continue;
        u8 pr[4] = { 0x05, (u8)a };
        T.props_done = 0;
        hbm(pr, 4);
        if (wait_for(&T.props_done, 1000)) continue;
        if (T.props_addr == a && !memcmp(T.props_guid, hid_guid, 16)) T.fw_hid = a;
    }
    if (!T.fw_hid) { strlcpy(status, "the sensor hub has no HID client", sizeof status); return -1; }
    u8 cc[4] = { 0x06, (u8)T.fw_hid, HOST_ADDR };
    hbm(cc, 4);
    if (wait_for(&T.connect_done, 2000) || T.connect_status) { fmt(status, sizeof status, "connecting to the HID client failed (%d)", T.connect_status); return -1; }
    flow_control();

    /* its HID devices */
    u8 buf[2048];
    int n = hid_call(H_ENUM, 0, NULL, 0, buf, sizeof buf);
    if (n < 1) { strlcpy(status, "the HID client listed no devices", sizeof status); return -1; }
    int ndev = MIN(buf[0], 4);
    int have_accel = 0, have_als = 0;
    for (int i = 0; i < ndev && 1 + 9 * i + 9 <= n; i++) {
        u32 dev;
        memcpy(&dev, buf + 1 + 9 * i, 4);
        int rl = hid_call(H_REPORT_DESC, (u8)dev, NULL, 0, buf + 512, (int)sizeof buf - 512);
        if (rl <= 0) continue;
        shub_t *h = &hubs[nhubs++];
        h->dev = (int)dev;
        parse_rdesc(h, buf + 512, rl);
        klog("sensors: device %u: %d-byte report descriptor, %d fields", dev, rl, h->nf);
        if (field(h, 0x200073, 1, 0x200453) && !sensor_on(h, 0x200073, 100)) have_accel = 1;
        if (field(h, 0x200041, 1, 0x2004d1) && !sensor_on(h, 0x200041, 1000)) have_als = 1;
    }
    fmt(status, sizeof status, "%s%s%s", have_accel ? "accelerometer" : "", have_accel && have_als ? ", " : "", have_als ? "ambient light" : "");
    if (!have_accel && !have_als) { strlcpy(status, "no accelerometer or light sensor found", sizeof status); return -1; }
    klog("sensors: %s on", status);
    return 0;
}

/* after bring-up: the reports */
static void sensor_step(void) {
    while (poll_once())
        if (T.msg_ready) {
            T.msg_ready = 0;
            if (T.msg_len >= 6 && (T.msg[0] & 0x7f) == H_INPUT) on_input(T.msg[1], T.msg + 6, T.msg_len - 6);
            else if (T.msg_len >= 10 && (T.msg[0] & 0x7f) == H_INPUT_LIST) {
                int nrep = T.msg[6 + 2], at = 6 + 4;
                for (int r = 0; r < nrep && at + 2 <= T.msg_len; r++) {
                    int rl = T.msg[at] | T.msg[at + 1] << 8;
                    if (at + 2 + rl > T.msg_len || rl < 6) break;
                    on_input(T.msg[at + 2 + 1], T.msg + at + 2 + 6, rl - 6);
                    at += 2 + rl;
                }
            }
        }
}

#ifndef ISH_HOST_TEST
u64 ish_now_ms(void) { return k_now_ms(); }
static void sensor_thread(void *arg) {
    (void)arg;
    if (!bringup()) for (;;) { sensor_step(); thread_sleep_ms(20); }
    klog("sensors: %s", status);
    thread_exit();
}
void ish_start(void) {
    if (!k.native || !k.is_venue) { strlcpy(status, k.native ? "not a Venue 8 Pro" : "firmware mode", sizeof status); return; }
    if (!hal_setting_get(u"QrtSensors", 1)) { strlcpy(status, "turned off (QrtSensors = 0)", sizeof status); return; }
    strlcpy(status, "starting", sizeof status);
    thread_create("sensors", sensor_thread, NULL, 0);
}
#endif
#else
void ish_start(void) {}
#endif
