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
 * Connections (0.9.2, for A2DP headphones): Create Connection, then
 * Authentication Requested; the controller asks for the link key (a stored
 * one, or none), and without one Secure Simple Pairing runs - we say we have
 * no input and no output, so it is "just works", the way headphones pair - or,
 * for old devices, legacy pairing with PIN 0000.  The new link key is kept in
 * NVRAM, encryption is switched on, and L2CAP (l2cap.c) takes over.  Devices
 * that connect to us (headphones reconnecting) are accepted the same way.
 *
 * Commands obey the controller's credit (Num_HCI_Command_Packets): replies
 * to pairing events go through a queue, sent as credits come back.  ACL data
 * obeys the buffer count from Read Buffer Size, returned by Number of
 * Completed Packets events.
 *
 * Events and ACL data can arrive in any context (the USB event loop runs with
 * interrupts off), so bt_rx_event/bt_rx_acl only queue them; this file's
 * thread handles them.
 */
#include "bt.h"

#define Q_N   32
#define EVT_MAX 260
#define ACL_Q   32
#define ACL_MAX 1100

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
    /* command credits and the queue of commands sent without waiting */
    volatile int ncmd;
    u8 cq[8][3 + 255];
    volatile u32 cqhead, cqtail;
    /* ACL data in (bt_rx_acl -> this thread) and its reassembly into L2CAP frames */
    u8 aq[ACL_Q][ACL_MAX];
    u16 aqlen[ACL_Q];
    volatile u32 aqhead, aqtail;
    u8 frame[4096];
    int frame_len, frame_want;
    u16 frame_handle;
    /* ACL data out */
    int acl_mtu, acl_total;
    volatile int acl_free;
    /* the connection (one: the headphones) */
    struct {
        int state;                       /* C_* */
        u16 handle;
        u8 addr[6];
        char name[48];
        int initiator, encrypted, paired_now;
        u64 since;
    } c;
    volatile int want_connect, want_disconnect;
    u8 want_addr[6];
    char audio_status[96];
} B;

enum { C_IDLE, C_CONNECTING, C_AUTH, C_ENCRYPT, C_UP };

int bt_state(void) { return B.state; }
int bt_in_bootloader(void) { return B.bootloader; }
const char *bt_status(void) {
    if (B.status[0]) return B.status;
    return k.native ? "No USB Bluetooth controller was found" : "Bluetooth needs the native kernel (Settings > Startup)";
}

/* ---- link keys (NVRAM: QrtBtKeys) ------------------------------------------------------ */
#define NKEYS 4
typedef struct { u8 addr[6]; u8 key[16]; char name[26]; } bt_key_t;
static bt_key_t keys[NKEYS];
static int keys_loaded;

static void keys_load(void) {
    if (keys_loaded) return;
    keys_loaded = 1;
    if (hal_setting_get_blob(u"QrtBtKeys", keys, sizeof keys) != sizeof keys) memset(keys, 0, sizeof keys);
}
static bt_key_t *key_find(const u8 *addr) {
    keys_load();
    static const u8 zero[6];
    if (!memcmp(addr, zero, 6)) return NULL;
    for (int i = 0; i < NKEYS; i++) if (!memcmp(keys[i].addr, addr, 6)) return &keys[i];
    return NULL;
}
static void key_store(const u8 *addr, const u8 *key, const char *name) {
    bt_key_t *k2 = key_find(addr);
    if (!k2) {                                             /* a free slot, else the oldest (the last) */
        memmove(&keys[1], &keys[0], sizeof keys[0] * (NKEYS - 1));
        k2 = &keys[0];
    }
    memcpy(k2->addr, addr, 6);
    memcpy(k2->key, key, 16);
    if (name && name[0]) strlcpy(k2->name, name, sizeof k2->name);
    hal_setting_set_blob(u"QrtBtKeys", keys, sizeof keys);
}
void bt_forget(const u8 addr[6]) {
    bt_key_t *k2 = key_find(addr);
    if (!k2) return;
    memset(k2, 0, sizeof *k2);
    hal_setting_set_blob(u"QrtBtKeys", keys, sizeof keys);
}

int bt_devices(bt_device_t *out, int max) {
    int n = 0;
    for (int i = 0; i < B.ndev && n < max; i++) { out[n] = B.dev[i]; out[n].paired = key_find(B.dev[i].addr) != NULL; n++; }
    keys_load();
    for (int i = 0; i < NKEYS && n < max; i++) {           /* paired devices the scan did not see */
        static const u8 zero[6];
        if (!memcmp(keys[i].addr, zero, 6)) continue;
        int seen = 0;
        for (int j = 0; j < n; j++) seen |= !memcmp(out[j].addr, keys[i].addr, 6);
        if (seen) continue;
        memset(&out[n], 0, sizeof out[n]);
        memcpy(out[n].addr, keys[i].addr, 6);
        strlcpy(out[n].name, keys[i].name[0] ? keys[i].name : "Paired device", sizeof out[n].name);
        out[n].named = 1;
        out[n].paired = 1;
        out[n].cod = 0x240404;                             /* shown as audio */
        n++;
    }
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

void bt_rx_acl(const u8 *pkt, int len) {
    if (len < 4 || len > ACL_MAX) return;
    u32 h = B.aqhead;
    if (h - B.aqtail >= ACL_Q) { B.dropped++; return; }
    memcpy(B.aq[h % ACL_Q], pkt, (usize)len);
    B.aqlen[h % ACL_Q] = (u16)len;
    B.aqhead = h + 1;
}

u64 bt_now_ms(void) { return B.t && B.t->now_ms ? B.t->now_ms() : 0; }

static void set_audio_status(const char *f, const char *name) {
    fmt(B.audio_status, sizeof B.audio_status, f, name && name[0] ? name : "the device");
    klog("bt: %s", B.audio_status);
}

void bt_set_audio_status(const char *f, const char *name) { set_audio_status(f, name); }

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

/* ---- commands sent without waiting (replies to pairing events) ----------------------------- */
static void cmd_async(u16 op, const void *param, int plen) {
    u32 h = B.cqhead;
    if (h - B.cqtail >= 8) { klog("bt: command queue full, %04x dropped", op); return; }
    u8 *c = B.cq[h % 8];
    c[0] = (u8)op; c[1] = (u8)(op >> 8); c[2] = (u8)plen;
    if (plen) memcpy(c + 3, param, (usize)plen);
    B.cqhead = h + 1;
}

static void cmd_pump(void) {
    while (B.cqtail != B.cqhead && B.ncmd > 0) {
        u8 *c = B.cq[B.cqtail % 8];
        B.ncmd--;
        B.t->send_cmd(c, 3 + c[2]);
        B.cqtail++;
    }
}

static void put_addr_cmd(u16 op, const u8 *addr) { cmd_async(op, addr, 6); }

/* ---- connections and pairing ----------------------------------------------------------------- */
static void conn_reset(void) { memset(&B.c, 0, sizeof B.c); }

static void link_up(void) {
    B.c.state = C_UP;
    klog("bt: %s connected%s, encrypted", B.c.name, B.c.paired_now ? " and paired" : "");
    l2cap_link_up(B.c.handle);
    a2dp_link_ready(B.c.handle, B.c.addr, B.c.name, B.c.initiator);
}

static void conn_event(u8 code, const u8 *p, int n) {
    switch (code) {
    case 0x03:                                                      /* Connection Complete */
        if (n < 11 || p[9] != 1) break;                             /* ACL links only */
        if (p[0]) {
            if (B.c.state == C_CONNECTING) { set_audio_status("Could not connect to %s", B.c.name); conn_reset(); }
            break;
        }
        memcpy(B.c.addr, p + 3, 6);
        B.c.handle = (u16)((p[1] | p[2] << 8) & 0x0fff);
        B.c.encrypted = p[10] != 0;
        if (!B.c.name[0]) { bt_device_t *d = dev_for(B.c.addr); bt_key_t *k2 = key_find(B.c.addr); strlcpy(B.c.name, d && d->named ? d->name : k2 && k2->name[0] ? k2->name : "Bluetooth device", sizeof B.c.name); }
        klog("bt: link to %s up (handle %u)", B.c.name, B.c.handle);
        B.c.state = C_AUTH;
        B.c.since = bt_now_ms();
        if (B.c.initiator) { u8 h2[2] = { (u8)B.c.handle, (u8)(B.c.handle >> 8) }; cmd_async(0x0411, h2, 2); }   /* Authentication Requested */
        break;
    case 0x04: {                                                    /* Connection Request: a device connects to us */
        if (n < 10 || p[9] != 1 || B.c.state != C_IDLE) {
            u8 r[7]; memcpy(r, p, 6); r[6] = 0x0d;                  /* busy: Reject (limited resources) */
            cmd_async(0x040a, r, 7);
            break;
        }
        conn_reset();
        B.c.state = C_CONNECTING;
        B.c.initiator = 0;
        memcpy(B.c.addr, p, 6);
        bt_device_t *d = dev_for(p);
        bt_key_t *k2 = key_find(p);
        strlcpy(B.c.name, d && d->named ? d->name : k2 && k2->name[0] ? k2->name : "Bluetooth device", sizeof B.c.name);
        set_audio_status("%s is connecting", B.c.name);
        u8 a[7]; memcpy(a, p, 6); a[6] = 0x00;                      /* accept, and become master */
        cmd_async(0x0409, a, 7);
        break;
    }
    case 0x05:                                                      /* Disconnection Complete */
        if (n >= 4 && (u16)((p[1] | p[2] << 8) & 0x0fff) == B.c.handle && B.c.state != C_IDLE) {
            klog("bt: %s disconnected (reason %02x)", B.c.name, p[3]);
            l2cap_link_down(B.c.handle);
            a2dp_link_down(B.c.handle);
            if (B.c.state != C_UP) set_audio_status(p[3] == 0x05 || p[3] == 0x06 ? "Pairing with %s failed" : "%s disconnected", B.c.name);
            else set_audio_status("%s disconnected", B.c.name);
            conn_reset();
        }
        break;
    case 0x06:                                                      /* Authentication Complete */
        if (n < 3 || B.c.state != C_AUTH) break;
        if (p[0]) { set_audio_status("Pairing with %s failed", B.c.name); hci_disconnect(B.c.handle); break; }
        if (B.c.encrypted) { link_up(); break; }
        B.c.state = C_ENCRYPT;
        { u8 e[3] = { (u8)B.c.handle, (u8)(B.c.handle >> 8), 1 }; cmd_async(0x0413, e, 3); }   /* Set Connection Encryption */
        break;
    case 0x08:                                                      /* Encryption Change */
        if (n < 4 || (u16)((p[1] | p[2] << 8) & 0x0fff) != B.c.handle) break;
        B.c.encrypted = !p[0] && p[3];
        if (B.c.encrypted && (B.c.state == C_ENCRYPT || B.c.state == C_AUTH)) link_up();
        else if (!B.c.encrypted && B.c.state != C_UP) { set_audio_status("%s would not encrypt the link", B.c.name); hci_disconnect(B.c.handle); }
        break;
    case 0x13:                                                      /* Number Of Completed Packets */
        if (n >= 1) for (int i = 0; i < p[0] && 1 + 4 * i + 4 <= n; i++) B.acl_free += p[1 + 4 * i + 2] | p[1 + 4 * i + 3] << 8;
        if (B.acl_free > B.acl_total) B.acl_free = B.acl_total;
        break;
    case 0x16: {                                                    /* PIN Code Request: legacy pairing, 0000 */
        if (n < 6) break;
        u8 r[23] = { 0 };
        memcpy(r, p, 6); r[6] = 4; memcpy(r + 7, "0000", 4);
        cmd_async(0x040d, r, 23);
        B.c.paired_now = 1;
        break;
    }
    case 0x17: {                                                    /* Link Key Request */
        if (n < 6) break;
        bt_key_t *k2 = key_find(p);
        if (k2) { u8 r[22]; memcpy(r, p, 6); memcpy(r + 6, k2->key, 16); cmd_async(0x040b, r, 22); }
        else put_addr_cmd(0x040c, p);                               /* no key: pair */
        break;
    }
    case 0x18:                                                      /* Link Key Notification: keep it */
        if (n >= 22) { key_store(p, p + 6, B.c.name); B.c.paired_now = 1; klog("bt: paired with %s", B.c.name); }
        break;
    case 0x31: {                                                    /* IO Capability Request */
        if (n < 6) break;
        u8 r[9]; memcpy(r, p, 6);
        r[6] = 0x03;                                                /* NoInputNoOutput: "just works", as headphones are */
        r[7] = 0x00;                                                /* no OOB data */
        r[8] = 0x04;                                                /* general bonding, MITM not required */
        cmd_async(0x042b, r, 9);
        if (B.c.state == C_CONNECTING || B.c.state == C_AUTH) set_audio_status("Pairing with %s", B.c.name);
        break;
    }
    case 0x33:                                                      /* User Confirmation Request: just works */
        if (n >= 6) put_addr_cmd(0x042c, p);
        break;
    case 0x36:                                                      /* Simple Pairing Complete */
        if (n >= 7 && p[0]) klog("bt: simple pairing failed (%02x)", p[0]);
        break;
    }
}

void hci_disconnect(u16 handle) {
    u8 d[3] = { (u8)handle, (u8)(handle >> 8), 0x13 };             /* remote user terminated */
    cmd_async(0x0406, d, 3);
}

/* ---- ACL ------------------------------------------------------------------------------------ */
static void drain_events_only(void);
int hci_acl_room(void) { return B.acl_free; }

/* one L2CAP frame, cut into ACL packets of the controller's size */
int hci_send_acl(u16 handle, const u8 *l2, int len) {
    if (!B.acl_mtu || B.c.state == C_IDLE) return -1;
    u8 pkt[4 + 1100];
    int mtu = MIN(B.acl_mtu, 1100), o = 0, first = 1;
    while (o < len) {
        if (B.acl_free <= 0) {                                      /* wait for the controller to send some */
            for (int t = 0; t < 200 && B.acl_free <= 0; t++) { if (B.t->poll) B.t->poll(); B.t->sleep_ms(1); drain_events_only(); }
            if (B.acl_free <= 0) return -2;
        }
        int n = MIN(len - o, mtu);
        u16 hf = (u16)(handle | (first ? 0x2000 : 0x1000));         /* PB: first (flushable) / continuing */
        pkt[0] = (u8)hf; pkt[1] = (u8)(hf >> 8); pkt[2] = (u8)n; pkt[3] = (u8)(n >> 8);
        memcpy(pkt + 4, l2 + o, (usize)n);
        B.acl_free--;
        if (B.t->send_bulk(pkt, 4 + n)) return -3;
        o += n;
        first = 0;
    }
    return 0;
}

/* ACL packets -> L2CAP frames (start, then continuations) */
static void acl_in(const u8 *a, int len) {
    u16 hf = (u16)(a[0] | a[1] << 8), dl = (u16)(a[2] | a[3] << 8);
    u16 handle = hf & 0x0fff;
    int pb = (hf >> 12) & 3;
    if (4 + dl > len) return;
    const u8 *d = a + 4;
    if (pb == 2 || pb == 0) {                                       /* start of a frame */
        if (dl < 4) return;
        B.frame_want = 4 + (d[0] | d[1] << 8);
        B.frame_len = 0;
        B.frame_handle = handle;
    } else if (pb != 1 || handle != B.frame_handle || !B.frame_want) return;
    if (B.frame_len + dl > (int)sizeof B.frame || B.frame_len + dl > B.frame_want) { B.frame_want = 0; return; }
    memcpy(B.frame + B.frame_len, d, dl);
    B.frame_len += dl;
    if (B.frame_len == B.frame_want) { B.frame_want = 0; l2cap_rx(handle, B.frame, B.frame_len); }
}

static void handle_event(const u8 *e, int len) {
    u8 code = e[0];
    const u8 *p = e + 2;
    int n = len - 2;
    switch (code) {
    case 0x0e:                                                      /* Command Complete */
        if (n >= 1) B.ncmd = p[0];
        if (n >= 3 && (p[1] | p[2] << 8) == B.wait_op) {
            B.ret_len = MIN(n - 3, EVT_MAX);
            memcpy(B.ret, p + 3, (usize)B.ret_len);
            B.got = 1;
        }
        break;
    case 0x0f:                                                      /* Command Status */
        if (n >= 2) B.ncmd = p[1];
        if (n >= 4 && (p[2] | p[3] << 8) == B.wait_op) { B.ret[0] = p[0]; B.ret_len = 1; B.got = 1; }
        if (n >= 4 && p[0] && (p[2] | p[3] << 8) == 0x0405 && B.c.state == C_CONNECTING) {   /* Create Connection refused */
            set_audio_status("Could not reach %s", B.c.name);
            B.c.state = C_IDLE;
        }
        break;
    default:
        conn_event(code, p, n);
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

/* events only: credits (Number of Completed Packets) while an ACL send waits */
static void drain_events_only(void) {
    while (B.qtail != B.qhead) {
        u32 i = B.qtail % Q_N;
        u8 *e = B.q[i];
        if (e[0] == 0x13 || e[0] == 0x0e || e[0] == 0x0f) handle_event(e, B.qlen[i]);
        else break;                                                /* the rest in order, by the thread's loop */
        B.qtail++;
    }
}

static void drain(void) {
    if (B.t->poll) B.t->poll();
    while (B.qtail != B.qhead) {
        u32 i = B.qtail % Q_N;
        handle_event(B.q[i], B.qlen[i]);
        B.qtail++;
    }
    while (B.aqtail != B.aqhead) {
        u32 i = B.aqtail % ACL_Q;
        acl_in(B.aq[i], B.aqlen[i]);
        B.aqtail++;
    }
    cmd_pump();
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
    for (int t = 0; t < 1000 && B.ncmd <= 0 && !(B.bootloader && op == 0xfc09); t++) { B.t->sleep_ms(1); drain(); }
    B.wait_op = op;
    B.got = 0;
    if (!(B.bootloader && op == 0xfc09) && B.ncmd > 0) B.ncmd--;
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
    B.ncmd = 1;
    B.state = BT_STARTING;
    strlcpy(B.status, "Starting...", sizeof B.status);
    /* Linux's btusb: only Intel's newer controllers (BTUSB_INTEL_NEW: 8260 0a2b, 0aaa, the 9x6x
     * series 0025..0040) start in a bootloader; 8087:07da (the Centrino Advanced-N 6235's) is
     * a CSR chip (BTUSB_CSR) and 07dc/0a2a run their ROM firmware: standard HCI */
    int intel_new = vid == 0x8087 && (pid == 0x0a2b || pid == 0x0aaa || (pid >= 0x0025 && pid <= 0x0040));
    if (intel_new && !intel_setup()) { B.state = BT_FAILED; klog("bt: %s", B.status); return; }
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
    /* for connections: buffers, Secure Simple Pairing, who we are, reachable by paired devices */
    if (cmd(0x1005, NULL, 0, r, sizeof r, 1000) >= 8 && !r[0]) {            /* Read Buffer Size */
        B.acl_mtu = r[1] | r[2] << 8;
        B.acl_total = B.acl_free = r[4] | r[5] << 8;
    }
    if (!B.acl_mtu) { B.acl_mtu = 310; B.acl_total = B.acl_free = 4; }
    u8 one = 1;
    cmd(0x0c56, &one, 1, NULL, 0, 1000);                                    /* Write Simple Pairing Mode */
    static const u8 cod[3] = { 0x1c, 0x01, 0x20 };                          /* computer: tablet; audio service */
    cmd(0x0c24, cod, 3, NULL, 0, 1000);                                     /* Write Class of Device */
    u8 policy[2] = { 0x05, 0x00 };                                          /* role switch, sniff */
    cmd(0x080f, policy, 2, NULL, 0, 1000);                                  /* Write Default Link Policy */
    u8 scan_en = 0x02;                                                      /* page scan: paired devices can connect */
    cmd(0x0c1a, &scan_en, 1, NULL, 0, 1000);                                /* Write Scan Enable */
    a2dp_init();
    const u8 *a = B.addr;
    fmt(B.status, sizeof B.status, "Ready, address %02X:%02X:%02X:%02X:%02X:%02X%s", a[5], a[4], a[3], a[2], a[1], a[0],
        intel_new ? " (Intel firmware loaded)" : "");
    klog("bt: %s, ACL %d x %d bytes", B.status, B.acl_total, B.acl_mtu);
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

/* ---- audio devices: the UI's requests ------------------------------------------------------ */
void bt_audio_connect(const u8 addr[6]) { memcpy(B.want_addr, addr, 6); B.want_disconnect = 0; B.want_connect = 1; }
void bt_audio_disconnect(void) { B.want_disconnect = 1; }
const char *bt_audio_status(void) { return B.audio_status; }

static void start_connect(void) {
    B.want_connect = 0;
    if (B.state != BT_READY) { set_audio_status("Bluetooth is busy (scanning); try again", NULL); return; }
    if (B.c.state != C_IDLE) {
        if (!memcmp(B.c.addr, B.want_addr, 6)) return;            /* already on it */
        hci_disconnect(B.c.handle);
        return;
    }
    conn_reset();
    memcpy(B.c.addr, B.want_addr, 6);
    bt_device_t *d = dev_for(B.want_addr);
    bt_key_t *k2 = key_find(B.want_addr);
    strlcpy(B.c.name, d && d->named ? d->name : k2 && k2->name[0] ? k2->name : "the device", sizeof B.c.name);
    B.c.initiator = 1;
    B.c.state = C_CONNECTING;
    B.c.since = bt_now_ms();
    set_audio_status("Connecting to %s", B.c.name);
    u8 cc2[13];
    memcpy(cc2, B.c.addr, 6);
    cc2[6] = 0x18; cc2[7] = 0xcc;                                  /* DM1 DH1 DM3 DH3 DM5 DH5 */
    cc2[8] = 0x01; cc2[9] = 0;                                     /* page scan R1 */
    cc2[10] = 0; cc2[11] = 0;                                      /* no clock offset */
    cc2[12] = 0x01;                                                /* role switch allowed */
    cmd_async(0x0405, cc2, 13);
}

void bt_poll_once(void) {
    drain();
    if (B.scan_req && B.state == BT_READY && B.c.state == C_IDLE) { B.scan_req = 0; scan(); }
    if (B.want_connect) start_connect();
    if (B.want_disconnect) {
        B.want_disconnect = 0;
        if (B.c.state != C_IDLE) hci_disconnect(B.c.handle);
    }
    /* a connection that never came up */
    if (B.c.state != C_IDLE && B.c.state != C_UP && bt_now_ms() - B.c.since > 30000) {
        set_audio_status("%s did not answer", B.c.name);
        if (B.c.state != C_CONNECTING) hci_disconnect(B.c.handle);
        else { cmd_async(0x0408, B.c.addr, 6); conn_reset(); }     /* Create Connection Cancel */
    }
    a2dp_poll();
}

void bt_run(void) {
    for (;;) {
        bt_poll_once();
        B.t->sleep_ms(B.c.state != C_IDLE ? 2 : 20);
    }
}
