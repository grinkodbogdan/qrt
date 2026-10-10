/*
 * lbt.c - Bluetooth through Linux on the Xiaomi Mi A1: the WCN3680's Bluetooth sits
 * behind the same Pronto core as Wi-Fi, and Linux's btqcomsmd makes it hci0 (HCI over
 * the SMD channels APPS_RIVA_BT_CMD / _ACL).  QRT keeps its own Bluetooth host stack
 * (src/drivers/bt: HCI, pairing, L2CAP, A2DP, SBC - the tablets' stack): this file is
 * its transport, as btusb.c is on USB.  hci0 is opened through Linux's HCI user channel
 * (HCI_CHANNEL_USER): Linux's Bluetooth core steps aside and every packet goes to QRT,
 * framed as on a UART (01 command, 02 ACL, 04 event).
 *
 *   - the Pronto core is started if Wi-Fi has not started it yet;
 *   - its Bluetooth comes up without an address (no local-bd-address in the tree): it
 *     gets one made from the eMMC's serial number, through the controller's NVM
 *     (Qualcomm's vendor command fc0b, tag 2 - Linux's qca_set_bdaddr_rome).
 */
#include "arm.h"
#include "sched.h"
#include "../../drivers/bt/bt.h"

#ifdef QRT_LKL
long argon_sys(long nr, long a, long b, long c, long d, long e);
long argon_sys6(long nr, long a, long b, long c, long d, long e, long f);
int linux_running(void);
int lwifi_core_start(void);

#define NR_SOCKET     198
#define NR_BIND       200
#define NR_SENDTO     206
#define NR_RECVFROM   207
#define NR_IOCTL      29
#define NR_OPENAT     56
#define NR_CLOSE      57
#define NR_READ       63
#define AT_FDCWD      (-100)
#define S(nr, a, b, c, d, e) argon_sys(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))
#define S6(nr, a, b, c, d, e, f) argon_sys6(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))
#define LOG(...) klog("bt: " __VA_ARGS__)

static long fd = -1;
static u8 rx[1200];

static int rd(const char *path, char *out, int cap) {
    long f = S(NR_OPENAT, AT_FDCWD, path, 0, 0, 0);
    if (f < 0) return -1;
    long n = S(NR_READ, f, out, cap - 1, 0, 0);
    S(NR_CLOSE, f, 0, 0, 0, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return (int)n;
}

static int send_pkt(u8 type, const u8 *p, int len) {
    static u8 b[1100];
    if (len + 1 > (int)sizeof b) return -1;
    b[0] = type;
    memcpy(b + 1, p, (usize)len);
    long r = S6(NR_SENDTO, fd, b, len + 1, 0, 0, 0);
    return r == len + 1 ? 0 : -1;
}
static int send_cmd(const u8 *p, int len) { return send_pkt(0x01, p, len); }
static int send_acl(const u8 *p, int len) { return send_pkt(0x02, p, len); }
/* packets from the controller: a thread waits in Linux for each (no polling - every call
 * into Linux costs a hand-over, and the stack's loop runs 50 times a second) */
static void reader(void *a) {
    (void)a;
    static u8 b[1200];
    for (;;) {
        long n = S6(NR_RECVFROM, fd, b, sizeof b, 0, 0, 0);
        if (n <= 1) { if (n < 0) thr_sleep_us(100000); continue; }
        if (b[0] == 0x04 && n >= 3) bt_rx_event(b + 1, (int)n - 1);
        else if (b[0] == 0x02 && n >= 5) bt_rx_acl(b + 1, (int)n - 1);
    }
}
static void poll(void) {}
static void sleep_ms(u32 ms) { thr_sleep_us((u64)ms * 1000); }
static u8 *read_file(const char *path, u64 *len) { (void)path; (void)len; return NULL; }
static void free_file(u8 *p) { (void)p; }
static u64 now_ms(void) { return k_now_ms(); }
static const bt_transport_t transport = { send_cmd, send_acl, poll, sleep_ms, read_file, free_file, now_ms };

/* one command, its answer awaited by hand (before the stack runs) */
static int raw_cmd(u16 op, const u8 *par, int n) {
    u8 c[64];
    c[0] = (u8)op; c[1] = (u8)(op >> 8); c[2] = (u8)n;
    memcpy(c + 3, par, (usize)n);
    if (send_cmd(c, 3 + n) < 0) return -1;
    for (int t = 0; t < 100; t++) {                                  /* 1 s */
        long r = S6(NR_RECVFROM, fd, rx, sizeof rx, 0x40, 0, 0);
        if (r >= 7 && rx[0] == 0x04 && rx[1] == 0x0e && (rx[4] | rx[5] << 8) == op) return rx[6];   /* Command Complete: status */
        if (r >= 3 && rx[0] == 0x04 && rx[1] == 0xff) return 0;       /* the vendor's own event */
        if (r <= 0) thr_sleep_us(10000);
    }
    return -2;
}

static void bt_loop(void *a) {
    (void)a;
    for (int t = 0; t < 240 && !linux_running(); t++) thr_sleep_us(500000);
    char t[64];
    /* hci0 appears once the Pronto core runs (btqcomsmd binds to its SMD channels) */
    int i = 0;
    for (; i < 60 && rd("/sys/class/bluetooth/hci0/uevent", t, sizeof t) < 0; i++) thr_sleep_us(1000000);
    if (i == 60) {
        LOG("starting the Pronto core (Wi-Fi had not)");
        lwifi_core_start();
        for (i = 0; i < 60 && rd("/sys/class/bluetooth/hci0/uevent", t, sizeof t) < 0; i++) thr_sleep_us(500000);
        if (i == 60) { LOG("no Bluetooth controller in Linux (hci0)"); return; }
    }
    fd = S(NR_SOCKET, 31, 3 | 0x80000, 1, 0, 0);                     /* AF_BLUETOOTH, SOCK_RAW | CLOEXEC, BTPROTO_HCI */
    if (fd < 0) { LOG("no HCI socket (%ld)", fd); return; }
    S(NR_IOCTL, fd, 0x400448ca, 0, 0, 0);                             /* HCIDEVDOWN hci0: Linux lets go of it */
    u8 sa[6] = { 31, 0, 0, 0, 1, 0 };                                 /* sockaddr_hci: hci0, HCI_CHANNEL_USER */
    long r = -1;
    for (i = 0; i < 20 && (r = S(NR_BIND, fd, sa, 6, 0, 0)) < 0; i++) thr_sleep_us(500000);   /* -EBUSY while Linux sets it up */
    if (r < 0) { LOG("hci0 would not open for QRT (%ld)", r); S(NR_CLOSE, fd, 0, 0, 0, 0); return; }
    LOG("hci0 (Linux's btqcomsmd, the Pronto core) - QRT's stack drives it");
    /* an address: the controller has none of its own */
    char cid[64] = "";
    if (rd("/sys/block/mmcblk0/device/cid", cid, sizeof cid) <= 0) strlcpy(cid, "qrt", sizeof cid);
    u64 h = 0x84222325cbf29ce4ull;                                    /* FNV-1a, another seed than Wi-Fi's */
    for (char *c = cid; *c && *c != '\n'; c++) { h ^= (u8)*c; h *= 0x100000001b3ull; }
    u8 nvm[9] = { 0x01, 0x02, 0x06 };                                 /* NVM set, tag 2 (BD address), 6 bytes */
    for (int k = 0; k < 6; k++) nvm[3 + k] = (u8)(h >> (8 * k));
    nvm[3 + 5] &= 0x3f;                                               /* not a multicast-looking top byte */
    int s = raw_cmd(0xfc0b, nvm, 9);
    if (s) LOG("could not set the Bluetooth address (%d)", s);
    thr_create("bluetooth rx", reader, NULL, 16 << 10);
    bt_attach(&transport, 0, 0);
    bt_run();
}

void lbt_start(void) {
    if (fdt_find_compatible(-1, "qcom,wcnss-bt") < 0) return;
    thr_create("bluetooth", bt_loop, NULL, 64 << 10);
}
#else
void lbt_start(void) {}
#endif
