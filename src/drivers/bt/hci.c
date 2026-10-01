/*
 * hci.c - QRT's Bluetooth core: the Host Controller Interface on top of a
 * transport (btusb.c), the Intel controller set-up, and scanning.
 *
 * The Intel set-up follows Linux's btusb.c (btusb_setup_intel_new) and
 * btintel.c: the Wireless 8260's Bluetooth starts in a bootloader that only
 * accepts signed firmware.  Read Version (0xfc05) says which state it is in
 * (firmware variant 0x06 = bootloader, 0x23 = operational); the boot
 * parameters (0xfc0d) give the device revision, which names the firmware
 * file ibt-<hw_variant>-<dev_revid>.sfi.  The file is sent with Secure Send
 * (0xfc09): the 128-byte CSS header, the 256-byte public key, the 256-byte
 * signature, then the command stream in fragments of up to 252 bytes that
 * end on 4-byte command boundaries.  Each fragment is acknowledged; a vendor
 * event (0xff, subcode 0x06) reports the result.  Intel Reset (0xfc01) boots
 * it - it sends no command complete - and vendor event 0x02 says it is up.
 * Then the DDC file (ibt-*.ddc) sets board parameters (0xfc8b).  While in
 * the bootloader, events arrive on the bulk endpoint and Secure Send goes
 * down the bulk endpoint too.
 *
 * After that it is a standard controller: Reset, Read BD_ADDR, a name, and
 * on request a scan: classic Inquiry with extended results (names come in
 * the EIR data, or by Remote Name Request afterwards) and an LE scan whose
 * advertising reports carry names in their AD data.
 *
 * Events can arrive in any context (the USB event loop runs with interrupts
 * off), so bt_rx_event only queues them; this file's thread handles them.
 */
#include "bt.h"

#define Q_N   32
#define EVT_MAX 260

static struct {
    const bt_transport_t *t;
    volatile int state;
    char status[112];
    u8 addr[6];
    int bootloader;
    /* event queue (bt_rx_event -> this thread) */
    u8 q[Q_N][EVT_MAX];
    u16 qlen[Q_N];
    volatile u32 qhead, qtail, dropped;
    /* command wait */
    u16 wait_op;
    int got;
    u8 ret[EVT_MAX];
    int ret_len;
    /* vendor notifications */
    volatile int fw_result, fw_done, booted;
    /* scanning */
    volatile int scan_req, inquiry_done;
    bt_device_t dev[BT_MAX_DEV];
    volatile int ndev;
} B;

int bt_state(void) { return B.state; }
int bt_in_bootloader(void) { return B.bootloader; }
const char *bt_status(void) {
    if (B.status[0]) return B.status;
    return k.native ? "No USB Bluetooth controller was found" : "Bluetooth needs the native kernel (Settings > Startup)";
}

int bt_devices(bt_device_t *out, int max) {
    int n = 0;
    for (int i = 0; i < B.ndev && n < max; i++) out[n++] = B.dev[i];
    return n;
}

void bt_scan(void) { if (B.state == BT_READY) B.scan_req = 1; }

const char *bt_kind(u32 cod, int le) {
    switch ((cod >> 8) & 0x1f) {
    case 1: return "Computer";
    case 2: return "Phone";
    case 3: return "Network";
    case 4: {
        int minor = (cod >> 2) & 0x3f;
        if (minor == 1 || minor == 2) return "Headset";
        if (minor == 6) return "Headphones";
        if (minor == 5 || minor == 7) return "Speaker";
        return "Audio";
    }
    case 5: return "Keyboard or mouse";
    case 6: return "Printer or camera";
    case 7: return "Watch";
    case 8: return "Toy";
    case 9: return "Health device";
    }
    return le ? "Bluetooth LE device" : "Device";
}

/* ---- events in --------------------------------------------------------------------- */
void bt_rx_event(const u8 *evt, int len) {
    if (len < 2 || len > EVT_MAX) return;
    u32 h = B.qhead;
    if (h - B.qtail >= Q_N) { B.dropped++; return; }
    memcpy(B.q[h % Q_N], evt, (usize)len);
    B.qlen[h % Q_N] = (u16)len;
    B.qhead = h + 1;
}

static bt_device_t *dev_for(const u8 *addr) {
    for (int i = 0; i < B.ndev; i++) if (!memcmp(B.dev[i].addr, addr, 6)) return &B.dev[i];
    if (B.ndev >= BT_MAX_DEV) return NULL;
    bt_device_t *d = &B.dev[B.ndev];
    memset(d, 0, sizeof *d);
    memcpy(d->addr, addr, 6);
    B.ndev++;
    return d;
}

/* EIR / advertising data: length, type, value... */
static void parse_name(bt_device_t *d, const u8 *p, int n) {
    for (int i = 0; i + 1 < n && p[i]; i += p[i] + 1) {
        int l = p[i], type = p[i + 1];
        if (i + 1 + l > n) break;
        if ((type == 0x09 || (type == 0x08 && !d->named)) && l > 1) {
            int m = MIN(l - 1, (int)sizeof d->name - 1);
            memcpy(d->name, p + i + 2, (usize)m);
            d->name[m] = 0;
            d->named = 1;
        }
    }
}

static void handle_event(const u8 *e, int len) {
    u8 code = e[0];
    const u8 *p = e + 2;
    int n = len - 2;
    switch (code) {
    case 0x0e:                                                      /* Command Complete */
        if (n >= 3 && (p[1] | p[2] << 8) == B.wait_op) {
            B.ret_len = MIN(n - 3, EVT_MAX);
            memcpy(B.ret, p + 3, (usize)B.ret_len);
            B.got = 1;
        }
        break;
    case 0x0f:                                                      /* Command Status */
        if (n >= 4 && (p[2] | p[3] << 8) == B.wait_op) { B.ret[0] = p[0]; B.ret_len = 1; B.got = 1; }
        break;
    case 0xff:                                                      /* Intel vendor events */
        if (n >= 1 && p[0] == 0x06) { B.fw_result = n >= 2 ? p[1] : 0; B.fw_done = 1; }
        else if (n >= 1 && p[0] == 0x02) B.booted = 1;
        break;
    case 0x01: B.inquiry_done = 1; break;                           /* Inquiry Complete */
    case 0x02: case 0x22: {                                         /* Inquiry Result (with RSSI) */
        int num = n > 0 ? p[0] : 0;
        for (int i = 0; i < num; i++) {
            /* arrays: addr[num], psrm[num], reserved(1 or 2)[num], cod[num], clock[num], (rssi[num]) */
            const u8 *a = p + 1 + 6 * i;
            int res = code == 0x02 ? 2 : 1;
            const u8 *cod = p + 1 + num * (6 + 1 + res) + 3 * i;
            if (cod + 3 > e + len) break;
            bt_device_t *d = dev_for(a);
            if (!d) break;
            d->cod = (u32)(cod[0] | cod[1] << 8 | cod[2] << 16);
            if (code == 0x22) d->rssi = (i8)p[1 + num * (6 + 1 + 1 + 3 + 2) + i];
        }
        break;
    }
    case 0x2f:                                                      /* Extended Inquiry Result */
        if (n >= 15) {
            bt_device_t *d = dev_for(p + 1);
            if (!d) break;
            d->cod = (u32)(p[9] | p[10] << 8 | p[11] << 16);
            d->rssi = (i8)p[14];
            parse_name(d, p + 15, n - 15);
        }
        break;
    case 0x07:                                                      /* Remote Name Request Complete */
        if (n >= 7) {
            bt_device_t *d = dev_for(p + 1);
            if (d && !p[0]) {
                int m = 0;
                while (m < n - 7 && m < (int)sizeof d->name - 1 && p[7 + m]) { d->name[m] = (char)p[7 + m]; m++; }
                d->name[m] = 0;
                d->named = m > 0;
            }
        }
        break;
    case 0x3e:                                                      /* LE Meta */
        if (n >= 2 && p[0] == 0x02) {                               /* advertising reports, one after another */
            int num = p[1], o = 2;
            for (int i = 0; i < num && o + 9 <= n; i++) {
                const u8 *a = p + o + 2;
                int dl = p[o + 8];
                if (o + 9 + dl + 1 > n) break;
                bt_device_t *d = dev_for(a);
                if (d) {
                    if (!d->cod) d->le = 1;
                    d->rssi = (i8)p[o + 9 + dl];
                    parse_name(d, p + o + 9, dl);
                }
                o += 10 + dl;
            }
        }
        break;
    }
}

static void drain(void) {
    if (B.t->poll) B.t->poll();
    while (B.qtail != B.qhead) {
        u32 i = B.qtail % Q_N;
        handle_event(B.q[i], B.qlen[i]);
        B.qtail++;
    }
}

static int wait_for(volatile int *flag, u32 ms) {
    for (u32 t = 0; !*flag; t++) {
        drain();
        if (*flag) break;
        if (t >= ms) return 0;
        B.t->sleep_ms(1);
    }
    return 1;
}

/* Send a command and wait for its Command Complete (or Status).  Returns the
 * return parameters' length (status byte first), < 0 on timeout. */
static int cmd(u16 op, const void *param, int plen, u8 *out, int cap, u32 ms) {
    u8 pkt[3 + 255];
    pkt[0] = (u8)op; pkt[1] = (u8)(op >> 8); pkt[2] = (u8)plen;
    if (plen) memcpy(pkt + 3, param, (usize)plen);
    drain();
    B.wait_op = op;
    B.got = 0;
    int e = (B.bootloader && op == 0xfc09) ? B.t->send_bulk(pkt, 3 + plen) : B.t->send_cmd(pkt, 3 + plen);
    if (e) { klog("bt: command %04x not sent (%d)", op, e); return -2; }
    if (op == 0xfc01 && B.bootloader) return 0;             /* Intel Reset: no command complete */
    if (!wait_for(&B.got, ms)) { klog("bt: command %04x timed out", op); return -1; }
    if (out) memcpy(out, B.ret, (usize)MIN(cap, B.ret_len));
    return B.ret_len;
}

/* ---- Intel controllers (btusb_setup_intel_new) ------------------------------------------- */
static int secure_send(u8 type, const u8 *p, u32 len) {
    while (len) {
        u8 frag[253];
        u32 n = len > 252 ? 252 : len;
        frag[0] = type;
        memcpy(frag + 1, p, n);
        u8 r[4];
        int rl = cmd(0xfc09, frag, (int)n + 1, r, sizeof r, 2000);
        if (rl < 1 || r[0]) return rl < 0 ? rl : -(int)r[0] - 100;
        p += n; len -= n;
    }
    return 0;
}

static int intel_setup(void) {
    u8 v[16];
    int vl = cmd(0xfc05, NULL, 0, v, sizeof v, 2000);
    if (vl < 10 || v[0]) { fmt(B.status, sizeof B.status, "Intel controller: no version (%d)", vl); return 0; }
    u8 hw_platform = v[1], hw_variant = v[2], fw_variant = v[4];
    klog("bt: Intel hw platform %02x variant %02x, firmware variant %02x revision %02x build %u-%u.%u",
         hw_platform, hw_variant, fw_variant, v[5], v[6], v[7], 2000 + v[8]);
    if (fw_variant == 0x23) { klog("bt: operational firmware already running"); return 1; }
    if (hw_platform != 0x37 || fw_variant != 0x06) {
        fmt(B.status, sizeof B.status, "Intel controller in an unknown state (platform %02x, firmware %02x)", hw_platform, fw_variant);
        return 0;
    }
    B.bootloader = 1;
    u8 bp[32];
    int bl = cmd(0xfc0d, NULL, 0, bp, sizeof bp, 2000);
    if (bl < 23 || bp[0]) { strlcpy(B.status, "Intel bootloader: no boot parameters", sizeof B.status); return 0; }
    u16 revid = (u16)(bp[4] | bp[5] << 8);                 /* struct intel_boot_params (btintel.h) */
    if (bp[21]) {                                          /* limited_cce */ strlcpy(B.status, "Intel bootloader without per-fragment acknowledgements", sizeof B.status); return 0; }
    char path[64];
    fmt(path, sizeof path, "/lib/firmware/ibt-%u-%u.sfi", hw_variant, revid);
    u64 fl = 0;
    u8 *fw = B.t->read_file(path, &fl);
    if (!fw || fl < 644) {
        fmt(B.status, sizeof B.status, "Bluetooth firmware %s missing", path + 14);
        if (fw) B.t->free_file(fw);
        return 0;
    }
    klog("bt: loading %s (%llu bytes), device revision %u, secure boot %s", path, fl, revid, bp[6] ? "on" : "off");
    B.fw_done = 0;
    int e = secure_send(0x00, fw, 128);                    /* CSS header */
    if (!e) e = secure_send(0x03, fw + 128, 256);          /* public key */
    if (!e) e = secure_send(0x02, fw + 388, 256);          /* signature */
    const u8 *ptr = fw + 644;
    u32 frag = 0;
    while (!e && ptr + frag < fw + fl) {                   /* commands, cut on 4-byte boundaries */
        frag += 3 + ptr[frag + 2];
        if (ptr + frag > fw + fl) { e = -3; break; }
        if (!(frag % 4)) { e = secure_send(0x01, ptr, frag); ptr += frag; frag = 0; }
    }
    B.t->free_file(fw);
    if (e) { fmt(B.status, sizeof B.status, "Bluetooth firmware download failed (%d)", e); return 0; }
    if (!wait_for(&B.fw_done, 5000) || B.fw_result) {
        fmt(B.status, sizeof B.status, "Bluetooth firmware %s", B.fw_done ? "rejected by the controller" : "download did not finish");
        return 0;
    }
    static const u8 reset_param[] = { 0x00, 0x01, 0x00, 0x01, 0x00, 0x08, 0x04, 0x00 };
    B.booted = 0;
    cmd(0xfc01, reset_param, sizeof reset_param, NULL, 0, 0);
    if (!wait_for(&B.booted, 2000)) { strlcpy(B.status, "Bluetooth firmware did not boot", sizeof B.status); return 0; }
    B.bootloader = 0;
    /* device configuration (DDC): length, id, value... each as one command */
    fmt(path, sizeof path, "/lib/firmware/ibt-%u-%u.ddc", hw_variant, revid);
    u8 *ddc = B.t->read_file(path, &fl);
    if (ddc) {
        for (u64 o = 0; o < fl && o + 1 + ddc[o] <= fl; o += 1 + ddc[o]) cmd(0xfc8b, ddc + o, 1 + ddc[o], NULL, 0, 1000);
        B.t->free_file(ddc);
    }
    static const u8 mask[8] = { 0x87, 0x0c, 0, 0, 0, 0, 0, 0 };
    cmd(0xfc52, mask, 8, NULL, 0, 1000);                   /* Intel event mask (no debug events) */
    klog("bt: Intel firmware running");
    return 1;
}

/* ---- set-up ------------------------------------------------------------------------------- */
void bt_attach(const bt_transport_t *t, u16 vid, u16 pid) {
    B.t = t;
    B.state = BT_STARTING;
    strlcpy(B.status, "Starting...", sizeof B.status);
    if (vid == 0x8087 && !intel_setup()) { B.state = BT_FAILED; klog("bt: %s", B.status); return; }
    u8 r[64];
    if (cmd(0x0c03, NULL, 0, r, sizeof r, 2000) < 1 || r[0]) {             /* Reset */
        strlcpy(B.status, "Bluetooth controller did not reset", sizeof B.status);
        B.state = BT_FAILED; return;
    }
    if (cmd(0x1009, NULL, 0, r, sizeof r, 1000) >= 7 && !r[0]) memcpy(B.addr, r + 1, 6);   /* Read BD_ADDR */
    static const u8 mask[8] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };          /* incl. LE meta, EIR */
    cmd(0x0c01, mask, 8, NULL, 0, 1000);                                    /* Set Event Mask */
    u8 name[248] = "QRT";
    cmd(0x0c13, name, sizeof name, NULL, 0, 1000);                          /* Write Local Name */
    u8 mode = 2;
    cmd(0x0c45, &mode, 1, NULL, 0, 1000);                                   /* Write Inquiry Mode: extended */
    u8 lemask[8] = { 0x1f, 0, 0, 0, 0, 0, 0, 0 };
    cmd(0x2001, lemask, 8, NULL, 0, 1000);                                  /* LE Set Event Mask */
    const u8 *a = B.addr;
    fmt(B.status, sizeof B.status, "Ready, address %02X:%02X:%02X:%02X:%02X:%02X%s", a[5], a[4], a[3], a[2], a[1], a[0],
        vid == 0x8087 ? " (Intel firmware loaded)" : "");
    klog("bt: %s", B.status);
    B.state = BT_READY;
}

static void scan(void) {
    B.state = BT_SCANNING;
    B.ndev = 0;
    B.inquiry_done = 0;
    static const u8 inq[5] = { 0x33, 0x8b, 0x9e, 0x08, 0x00 };             /* GIAC, 8 x 1.28 s, unlimited */
    int ok_classic = cmd(0x0401, inq, 5, NULL, 0, 1000) >= 1;
    static const u8 lep[7] = { 0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 };  /* active, 10 ms interval/window */
    cmd(0x200b, lep, 7, NULL, 0, 1000);
    static const u8 len_on[2] = { 0x01, 0x01 }, len_off[2] = { 0x00, 0x00 };
    cmd(0x200c, len_on, 2, NULL, 0, 1000);
    for (int t = 0; t < 12000 && !(ok_classic && B.inquiry_done && t > 10000); t += 10) { drain(); B.t->sleep_ms(10); }
    cmd(0x200c, len_off, 2, NULL, 0, 1000);
    if (!B.inquiry_done) cmd(0x0402, NULL, 0, NULL, 0, 1000);              /* Inquiry Cancel */
    /* classic devices without a name: ask them */
    for (int i = 0; i < B.ndev; i++) {
        bt_device_t *d = &B.dev[i];
        if (d->named || d->le) continue;
        u8 rq[10];
        memcpy(rq, d->addr, 6);
        rq[6] = 0x01; rq[7] = 0; rq[8] = 0; rq[9] = 0;                      /* page scan R1, no clock offset */
        if (cmd(0x0419, rq, 10, NULL, 0, 1000) < 1) continue;
        for (int t = 0; t < 5000 && !d->named; t += 10) { drain(); B.t->sleep_ms(10); }
    }
    klog("bt: scan found %d device%s", B.ndev, B.ndev == 1 ? "" : "s");
    B.state = BT_READY;
}

void bt_run(void) {
    for (;;) {
        drain();
        if (B.scan_req && B.state == BT_READY) { B.scan_req = 0; scan(); }
        B.t->sleep_ms(20);
    }
}
