/*
 * lwifi.c - Wi-Fi through Linux's own driver: on the Xiaomi Mi A1 the WCN3680 behind the
 * Pronto core (wcn36xx, Linux's mac80211), in QEMU mac80211_hwsim.  wlan.c's backend
 * (wlan.h: wlan_softmac_t): wlan.c keeps the network list, the password, the WPA2 4-way
 * handshake and the IP side; this file
 *   - starts the radio: on the phone the Pronto core's firmware comes from the modem
 *     partition, its calibration (the NV file) from vendor or persist, and it is started
 *     through the secure world - only when Wi-Fi is turned on;
 *   - talks nl80211 (generic netlink) to scan, connect, install the keys and open the
 *     port once the handshake is done - what wpa_supplicant does on Linux;
 *   - carries Ethernet frames, EAPOL included, over a packet socket on wlan0.
 * Linux gives wlan0 no address: IP is Tessera's (net.c), Linux only moves frames.
 */
#include "arm.h"
#include "sched.h"
#include "../../net/wlan.h"
#include "../../net/crypto.h"

#ifdef QRT_LKL
long argon_sys(long nr, long a, long b, long c, long d, long e);
long argon_sys6(long nr, long a, long b, long c, long d, long e, long f);
const char *argon_part_path(const char *part);
int argon_remoteproc_start(const char *fw);
int linux_running(void);

#define NR_SOCKET     198
#define NR_BIND       200
#define NR_SENDTO     206
#define NR_RECVFROM   207
#define NR_SETSOCKOPT 208
#define NR_IOCTL      29
#define NR_OPENAT     56
#define NR_CLOSE      57
#define NR_READ       63
#define NR_WRITE      64
#define NR_MKDIRAT    34
#define NR_GETDENTS64 61
#define AT_FDCWD      (-100)
#define S(nr, a, b, c, d, e) argon_sys(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))
#define S6(nr, a, b, c, d, e, f) argon_sys6(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

#define LOG(...) klog("wifi: " __VA_ARGS__)

/* nl80211 (include/uapi/linux/nl80211.h of the Linux built in) */
enum { C_GET_INTERFACE = 5, C_NEW_STATION = 19, C_NEW_KEY = 11, C_SET_STATION = 18, C_GET_SCAN = 32, C_TRIGGER_SCAN = 33, C_NEW_SCAN_RESULTS = 34,
       C_SCAN_ABORTED = 35, C_CONNECT = 46, C_DISCONNECT = 48 };
enum { A_IFINDEX = 3, A_MAC = 6, A_KEY_DATA = 7, A_KEY_IDX = 8, A_KEY_CIPHER = 9, A_KEY_SEQ = 10, A_KEY_DEFAULT = 11, A_WIPHY_FREQ = 38,
       A_IE = 42, A_SCAN_SSIDS = 45, A_BSS = 47, A_SSID = 52, A_AUTH_TYPE = 53, A_REASON_CODE = 54, A_KEY_TYPE = 55,
       A_STA_FLAGS2 = 67, A_CONTROL_PORT = 68, A_PRIVACY = 70, A_STATUS_CODE = 72, A_CIPHERS_PAIRWISE = 73,
       A_CIPHER_GROUP = 74, A_WPA_VERSIONS = 75, A_AKM_SUITES = 76, A_CONTROL_PORT_ETHERTYPE = 102 };
enum { B_BSSID = 1, B_FREQUENCY = 2, B_BEACON_INTERVAL = 4, B_CAPABILITY = 5, B_IES = 6, B_SIGNAL_MBM = 7, B_BEACON_IES = 11 };
#define CIPHER_CCMP 0x000fac04u
#define AKM_PSK     0x000fac02u

static int nlc = -1, nle = -1, pkt = -1;           /* netlink: requests, events; the packet socket */
static int family;                                  /* nl80211's generic netlink id */
static int ifindex;
static char ifname[16];
static u8 mac[6];
static int up, scanning;
static u32 seq = 1;

/* ---- netlink messages ---- */
static u8 mb[4096];
static int ml, nest[4], nn;
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 get32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void msg(u16 type, u16 flags, u8 cmd) {
    memset(mb, 0, 20);
    put16(mb + 4, type);
    put16(mb + 6, (u16)(1 | flags));                                  /* NLM_F_REQUEST */
    put32(mb + 8, ++seq);
    mb[16] = cmd; mb[17] = 1;                                         /* genl: command, version */
    ml = 20;
}
static void attr(u16 t, const void *d, int l) {
    if (ml + 4 + l + 3 > (int)sizeof mb) return;
    put16(mb + ml, (u16)(4 + l)); put16(mb + ml + 2, t);
    if (l) memcpy(mb + ml + 4, d, (usize)l);
    ml += 4 + l;
    while (ml & 3) mb[ml++] = 0;
}
static void attr32(u16 t, u32 v) { u8 b[4]; put32(b, v); attr(t, b, 4); }
static void attr16(u16 t, u16 v) { u8 b[2]; put16(b, v); attr(t, b, 2); }
static void attr8(u16 t, u8 v) { attr(t, &v, 1); }
static void nest_begin(u16 t) { nest[nn++] = ml; put16(mb + ml + 2, (u16)(t | 0x8000)); ml += 4; }
static void nest_end(void) { int o = nest[--nn]; put16(mb + o, (u16)(ml - o)); }

static int nl_open(void) {
    int fd = (int)S(NR_SOCKET, 16, 3, 16, 0, 0);                    /* AF_NETLINK, SOCK_RAW, NETLINK_GENERIC */
    if (fd < 0) return fd;
    u8 sa[12] = { 16, 0 };
    S(NR_BIND, fd, sa, 12, 0, 0);
    return fd;
}
/* the attributes of a message (after the genl header), or of a nested attribute */
typedef struct { const u8 *p[128]; u16 l[128]; } attrs_t;
static void parse(attrs_t *a, const u8 *p, int len) {
    memset(a, 0, sizeof *a);
    while (len >= 4) {
        int al = get16(p), t = get16(p + 2) & 0x3fff;
        if (al < 4 || al > len) break;
        if (t < 128) { a->p[t] = p + 4; a->l[t] = (u16)(al - 4); }
        int step = (al + 3) & ~3;
        p += step; len -= step;
    }
}

/* a request and its answer: 0, -errno, or what cb returns; cb sees each reply message */
static u8 rb[32768];
static int nl_call_on(int fd, int (*cb)(const u8 *m, int len, void *ctx), void *ctx) {
    u32 want = seq;
    put32(mb, (u32)ml);
    u8 sa[12] = { 16, 0 };
    if (S6(NR_SENDTO, fd, mb, ml, 0, sa, 12) < 0) return -1;
    for (int rounds = 0; rounds < 200; rounds++) {
        long n = S6(NR_RECVFROM, fd, rb, sizeof rb, 0, 0, 0);
        if (n <= 0) return -1;
        for (long o = 0; o + 16 <= n;) {
            u32 len = get32(rb + o);
            u16 type = get16(rb + o + 4);
            if (len < 16 || o + len > (u32)n) break;
            if (get32(rb + o + 8) == want) {
                if (type == 2) return (int)(i32)get32(rb + o + 16);       /* NLMSG_ERROR: 0 is the ACK */
                if (type == 3) return 0;                                  /* NLMSG_DONE */
                if (cb) { int r = cb(rb + o, (int)len, ctx); if (r) return r; }
            }
            o += (len + 3) & ~3u;
        }
    }
    return -1;
}

static int nl_call(int (*cb)(const u8 *m, int len, void *ctx), void *ctx) { return nl_call_on(nlc, cb, ctx); }

/* ---- nl80211's id and its multicast groups ---- */
struct fam { int id; int scan, mlme; };
static int fam_cb(const u8 *m, int len, void *ctx) {
    struct fam *f = ctx;
    attrs_t a;
    parse(&a, m + 20, len - 20);
    if (a.p[1]) f->id = get16(a.p[1]);                                  /* CTRL_ATTR_FAMILY_ID */
    if (a.p[7]) {                                                     /* CTRL_ATTR_MCAST_GROUPS */
        const u8 *p = a.p[7]; int l = a.l[7];
        while (l >= 4) {
            int gl = get16(p);
            if (gl < 4 || gl > l) break;
            attrs_t g;
            parse(&g, p + 4, gl - 4);
            if (g.p[1] && g.p[2]) {
                if (!strcmp((const char *)g.p[1], "scan")) f->scan = (int)get32(g.p[2]);
                if (!strcmp((const char *)g.p[1], "mlme")) f->mlme = (int)get32(g.p[2]);
            }
            int step = (gl + 3) & ~3;
            p += step; l -= step;
        }
    }
    return 0;
}
static int nl80211_open(void) {
    if (family) return 0;
    nlc = nl_open();
    nle = nl_open();
    if (nlc < 0 || nle < 0) { LOG("no netlink sockets (%d, %d)", nlc, nle); return -1; }
    msg(0x10, 4, 3);                                                  /* GENL_ID_CTRL, CTRL_CMD_GETFAMILY, with an ACK */
    attr(2, "nl80211", 8);
    struct fam f = { 0, 0, 0 };
    int r = nl_call(fam_cb, &f);
    if (r < 0 || !f.id) { LOG("Linux has no nl80211 (%d)", r); return -1; }
    family = f.id;
    S(NR_SETSOCKOPT, nle, 270, 1, &f.scan, 4);                        /* SOL_NETLINK, NETLINK_ADD_MEMBERSHIP */
    S(NR_SETSOCKOPT, nle, 270, 1, &f.mlme, 4);
    return 0;
}

/* ---- files in Linux ---- */
static int rd(const char *path, char *out, int cap) {
    long fd = S(NR_OPENAT, AT_FDCWD, path, 0, 0, 0);
    if (fd < 0) return -1;
    long n = S(NR_READ, fd, out, cap - 1, 0, 0);
    S(NR_CLOSE, fd, 0, 0, 0, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return (int)n;
}
static void wr(const char *path, const char *text) {
    long fd = S(NR_OPENAT, AT_FDCWD, path, 1, 0, 0);
    if (fd < 0) return;
    S(NR_WRITE, fd, text, strlen(text), 0, 0);
    S(NR_CLOSE, fd, 0, 0, 0, 0);
}
static long copy_file(const char *from, const char *to) {
    long in = S(NR_OPENAT, AT_FDCWD, from, 0, 0, 0);
    if (in < 0) return in;
    long out = S(NR_OPENAT, AT_FDCWD, to, 01101, 0644, 0);           /* O_WRONLY | O_CREAT | O_TRUNC */
    if (out < 0) { S(NR_CLOSE, in, 0, 0, 0, 0); return out; }
    static u8 b[16384];
    long total = 0, n;
    while ((n = S(NR_READ, in, b, sizeof b, 0, 0)) > 0) { S(NR_WRITE, out, b, n, 0, 0); total += n; }
    S(NR_CLOSE, in, 0, 0, 0, 0);
    S(NR_CLOSE, out, 0, 0, 0, 0);
    return total;
}

/* the first wireless interface (one with a phy80211 link) */
static int find_wlan(void) {
    long fd = S(NR_OPENAT, AT_FDCWD, "/sys/class/net", 0200000, 0, 0);
    if (fd < 0) return 0;
    static char d[4096];
    long n = S(NR_GETDENTS64, fd, d, sizeof d, 0, 0);
    S(NR_CLOSE, fd, 0, 0, 0, 0);
    char best[16] = "";
    for (long o = 0; o < n; o += *(u16 *)(d + o + 16)) {
        const char *nm = d + o + 19;
        if (nm[0] == '.') continue;
        char p[96];
        fmt(p, sizeof p, "/sys/class/net/%s/phy80211", nm);
        long f = S(NR_OPENAT, AT_FDCWD, p, 0200000, 0, 0);
        if (f < 0) continue;
        S(NR_CLOSE, f, 0, 0, 0, 0);
        if (!best[0] || strcmp(nm, best) < 0) strlcpy(best, nm, sizeof best);
    }
    if (!best[0]) return 0;
    strlcpy(ifname, best, sizeof ifname);
    char p[96], t[64];
    fmt(p, sizeof p, "/sys/class/net/%s/ifindex", ifname);
    if (rd(p, t, sizeof t) <= 0) return 0;
    ifindex = 0;
    for (char *c = t; *c >= '0' && *c <= '9'; c++) ifindex = ifindex * 10 + (*c - '0');
    fmt(p, sizeof p, "/sys/class/net/%s/address", ifname);
    if (rd(p, t, sizeof t) >= 17)
        for (int i = 0; i < 6; i++) {
            int v = 0;
            for (int j = 0; j < 2; j++) { char c = t[i * 3 + j]; v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); }
            mac[i] = (u8)v;
        }
    return ifindex > 0;
}

/* the Mi A1: the Pronto core's calibration where wcn36xx asks for it, then the core */
static const char *const nv_from[][2] = {
    { "vendor_a", "firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin" }, { "vendor_b", "firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin" },
    { "vendor_a", "etc/wifi/WCNSS_qcom_wlan_nv.bin" }, { "vendor", "firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin" },
    { "persist", "WCNSS_qcom_wlan_nv.bin" }, { "system_a", "vendor/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin" },
    { "system_a", "system/vendor/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin" }, { "system_a", "system/etc/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin" },
};
static int start_pronto(void) {
    if (fdt_find_compatible(-1, "qcom,pronto") < 0 && fdt_find_compatible(-1, "qcom,pronto-v3-pil") < 0) return -1;
    for (int t = 0; t < 40 && !argon_part_path("modem_a") && !argon_part_path("modem"); t++) thr_sleep_us(500000);
    if (!argon_part_path("modem_a") && !argon_part_path("modem")) { LOG("the modem partition (its firmware) is not mounted"); return -1; }
    S(NR_MKDIRAT, AT_FDCWD, "/lib", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware/wlan", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware/wlan/prima", 0755, 0, 0);
    long got = -1;
    for (usize i = 0; i < ARRAY_LEN(nv_from) && got <= 0; i++) {
        const char *m = argon_part_path(nv_from[i][0]);
        if (!m) continue;
        char p[160];
        fmt(p, sizeof p, "%s/%s", m, nv_from[i][1]);
        got = copy_file(p, "/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin");
        if (got > 0) LOG("calibration from %s/%s (%ld bytes)", nv_from[i][0], nv_from[i][1], got);
    }
    if (got <= 0) LOG("no WCNSS_qcom_wlan_nv.bin found (vendor, persist, system): the radio may not start");
    int r = argon_remoteproc_start("wcnss");
    if (r <= 0) {
        LOG("%s", r ? "the Wi-Fi core did not start" : "no Wi-Fi core (remoteproc) in Linux");
        void argon_wifi_report(void);
        argon_wifi_report();                                           /* why: Linux's lines, the devices, the processors */
        return -1;
    }
    return 0;
}

/* interface up: mac80211 starts the radio (wcn36xx loads its firmware) */
static int if_up_name(const char *name, int on) {
    long fd = S(NR_SOCKET, 2, 2, 0, 0, 0);                             /* AF_INET, SOCK_DGRAM */
    if (fd < 0) return -1;
    u8 ifr[40];
    memset(ifr, 0, sizeof ifr);
    strlcpy((char *)ifr, name, 16);
    S(NR_IOCTL, fd, 0x8913, ifr, 0, 0);                                /* SIOCGIFFLAGS */
    u16 fl = get16(ifr + 16);
    fl = on ? (u16)(fl | 1) : (u16)(fl & ~1);                           /* IFF_UP */
    put16(ifr + 16, fl);
    long r = S(NR_IOCTL, fd, 0x8914, ifr, 0, 0);                       /* SIOCSIFFLAGS */
    S(NR_CLOSE, fd, 0, 0, 0, 0);
    return r < 0 ? (int)r : 0;
}
static int if_up(int on) { return if_up_name(ifname, on); }

/* ---- frames: the packet socket, read by a thread, handed over in poll ---- */
#define RXQ 64
static struct { u16 len; u8 f[1600]; } rxq[RXQ];
static volatile u32 rq_head, rq_tail;
static void rx_loop(void *a) {
    (void)a;
    static u8 b[2048];
    u8 sll[20];
    for (;;) {
        int sl = sizeof sll;
        long n = S6(NR_RECVFROM, pkt, b, sizeof b, 0, sll, &sl);
        if (n <= 0) { thr_sleep_us(20000); continue; }
        if (sll[10] == 4) continue;                                    /* PACKET_OUTGOING: our own */
        if (n > 1600 || rq_head - rq_tail >= RXQ) continue;
        u64 f = irq_save();
        rxq[rq_head % RXQ].len = (u16)n;
        memcpy(rxq[rq_head % RXQ].f, b, (usize)n);
        rq_head++;
        irq_restore(f);
    }
}
static int pkt_open(void) {
    if (pkt >= 0) return 0;
    pkt = (int)S(NR_SOCKET, 17, 3, 0x0300, 0, 0);                       /* AF_PACKET, SOCK_RAW, htons(ETH_P_ALL) */
    if (pkt < 0) { LOG("no packet socket (%d)", pkt); return -1; }
    u8 sll[20];
    memset(sll, 0, sizeof sll);
    put16(sll, 17); sll[2] = 0x00; sll[3] = 0x03;                       /* family; protocol (big-endian) */
    put32(sll + 4, (u32)ifindex);
    if (S(NR_BIND, pkt, sll, 20, 0, 0) < 0) { LOG("packet socket: bind failed"); return -1; }
    thr_create("wifi rx", rx_loop, NULL, 32 << 10);
    return 0;
}

/* ---- the backend ---- */
static int lw_present(void) {
    if (!linux_running()) return 0;
    return fdt_find_compatible(-1, "qcom,pronto-v3-pil") >= 0 || fdt_find_compatible(-1, "qcom,pronto") >= 0 || find_wlan();
}

/* ---- QEMU: a test access point on hwsim's second radio (qrt.wifitest) ----
 * What hostapd does, cut to an open network: beacons from a template, authentication and
 * association answered by hand, the client added as an authorized station; then a frame
 * every second to the client, and what the client sends is counted. */
static int testing, testing_wpa, ap_if, ap_pkt = -1, ap_sock = -1;
static u8 ap_mac[6];
static volatile int ap_assoc;
static const u8 rsn_ie[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
/* the WPA2 side of the test AP: the authenticator of the 4-way handshake */
static u8 ap_pmk[32], ap_ptk[48], ap_anonce[32], ap_gtk[16], sta_mac[6];
static void ap_eapol(u16 info, u64 replay, const u8 *kd, usize kdl) {
    u8 e[14 + 4 + 95 + 64];
    usize len = 4 + 95 + kdl;
    memset(e, 0, sizeof e);
    memcpy(e, sta_mac, 6); memcpy(e + 6, ap_mac, 6); e[12] = 0x88; e[13] = 0x8e;
    u8 *f = e + 14, *k = f + 4;
    f[0] = 2; f[1] = 3; f[2] = (u8)((95 + kdl) >> 8); f[3] = (u8)(95 + kdl);
    k[0] = 2; k[1] = (u8)(info >> 8); k[2] = (u8)info; k[4] = 16;
    for (int i = 0; i < 8; i++) k[5 + i] = (u8)(replay >> (56 - 8 * i));
    memcpy(k + 13, ap_anonce, 32);
    k[93] = (u8)(kdl >> 8); k[94] = (u8)kdl;
    if (kdl) memcpy(k + 95, kd, kdl);
    if (info & 0x100) { u8 h[20]; hmac_sha1(ap_ptk, 16, f, len, h); memcpy(k + 77, h, 16); }
    S6(NR_SENDTO, ap_pkt, e, 14 + len, 0, 0, 0);
}
static void ap_key(const u8 *m, int idx, const u8 *key) {
    msg((u16)family, 4, C_NEW_KEY);
    attr32(A_IFINDEX, (u32)ap_if);
    if (m) attr(A_MAC, m, 6);
    attr8(A_KEY_IDX, (u8)idx);
    attr(A_KEY_DATA, key, 16);
    attr32(A_KEY_CIPHER, CIPHER_CCMP);
    if (!m) attr32(A_KEY_TYPE, 0);
    int r = nl_call_on(ap_sock, NULL, NULL);
    if (!m) {                                                          /* the group key is the AP's default for broadcasts */
        msg((u16)family, 4, 10);                                       /* SET_KEY */
        attr32(A_IFINDEX, (u32)ap_if);
        attr8(A_KEY_IDX, (u8)idx);
        attr(A_KEY_DEFAULT, NULL, 0);
        nl_call_on(ap_sock, NULL, NULL);
    }
    LOG("test AP: %s key installed (%d)", m ? "pairwise" : "group", r);
}
static void ap_eapol_in(const u8 *f, usize len) {
    if (len < 14 + 4 + 95) return;
    const u8 *k = f + 14 + 4;
    u16 info = (u16)(k[1] << 8 | k[2]);
    if ((info & 0x100) && !(info & 0x200)) {                          /* message 2: the SNonce */
        u8 data[76];
        const u8 *snonce = k + 13;
        int a_first = memcmp(ap_mac, sta_mac, 6) < 0;
        memcpy(data, a_first ? ap_mac : sta_mac, 6); memcpy(data + 6, a_first ? sta_mac : ap_mac, 6);
        int n_first = memcmp(ap_anonce, snonce, 32) < 0;
        memcpy(data + 12, n_first ? ap_anonce : snonce, 32); memcpy(data + 44, n_first ? snonce : ap_anonce, 32);
        prf_sha1(ap_pmk, 32, "Pairwise key expansion", data, sizeof data, ap_ptk, 48);
        u8 copy[256], h[20];
        usize fl = len - 14;
        if (fl > sizeof copy) return;
        memcpy(copy, f + 14, fl);
        memset(copy + 4 + 77, 0, 16);
        hmac_sha1(ap_ptk, 16, copy, fl, h);
        LOG("test AP: message 2, MIC %s", memcmp(h, k + 77, 16) ? "WRONG" : "right");
        u8 kd[48];
        memcpy(kd, rsn_ie, 22);
        static const u8 kde[8] = { 0xdd, 22, 0x00, 0x0f, 0xac, 1, 1, 0 };   /* GTK KDE: key id 1 */
        memcpy(kd + 22, kde, 8); memcpy(kd + 30, ap_gtk, 16);
        kd[46] = 0xdd; kd[47] = 0;
        u8 wrapped[56];
        aes_wrap(ap_ptk + 16, kd, 48, wrapped);
        ap_eapol(2 | 8 | 0x40 | 0x80 | 0x100 | 0x200 | 0x1000, 2, wrapped, 56);   /* message 3 */
    } else if ((info & 0x100) && (info & 0x200) && (info & 8)) {        /* message 4: keys in, port open */
        LOG("test AP: message 4 - the handshake is done");
        ap_key(sta_mac, 0, ap_ptk + 32);
        ap_key(NULL, 1, ap_gtk);
        u8 fl[8];
        put32(fl, 1u << 1); put32(fl + 4, 1u << 1);
        msg((u16)family, 4, C_SET_STATION);
        attr32(A_IFINDEX, (u32)ap_if);
        attr(A_MAC, sta_mac, 6);
        attr(A_STA_FLAGS2, fl, 8);
        nl_call_on(ap_sock, NULL, NULL);
        ap_assoc = 2;
    }
}
static u32 rd_u32(const char *p) { char t[32]; u32 v = 0; if (rd(p, t, sizeof t) > 0) for (char *c = t; *c >= '0' && *c <= '9'; c++) v = v * 10 + (u32)(*c - '0'); return v; }
static void ap_frame_out(const u8 *f, int len) {
    msg((u16)family, 4, 59);                                           /* NL80211_CMD_FRAME */
    attr32(A_IFINDEX, (u32)ap_if);
    attr(51, f, len);                                                   /* NL80211_ATTR_FRAME */
    attr(142, NULL, 0);                                                 /* DONT_WAIT_FOR_ACK */
    nl_call_on(ap_sock, NULL, NULL);
}
static void ap_loop(void *a) {
    (void)a;
    static u8 b[8192];
    int told = 0;
    for (;;) {
        long n = S6(NR_RECVFROM, ap_sock, b, sizeof b, 0x40, 0, 0);
        if (n <= 0) {
            if (ap_assoc && ap_pkt >= 0) {                                 /* the client's frames, and one to it */
                static u8 r[2048];
                long m;
                static u16 types[8];
                u8 sll[20];
                int sl = sizeof sll;
                while ((m = S6(NR_RECVFROM, ap_pkt, r, sizeof r, 0x40, sll, &sl)) > 0) {
                    sl = sizeof sll;
                    if (sll[10] == 4) continue;                                /* the AP's own */
                    u16 ty = (u16)(r[12] << 8 | r[13]);
                    if (ty == 0x888e) { ap_eapol_in(r, (usize)m); continue; }
                    int k = 0;
                    while (k < told && types[k] != ty) k++;
                    if (m >= 14 && k == told && told < 8) { types[told++] = ty; LOG("test AP: a frame from the client (type %04x, %ld bytes)", ty, m); }
                }
                u8 e[60];
                memset(e, 0, sizeof e);
                memset(e, 0xff, 6); memcpy(e + 6, ap_mac, 6); e[12] = 0x88; e[13] = 0xb5;
                memcpy(e + 14, "QRT test frame", 14);
                if (!testing_wpa || ap_assoc == 2) S6(NR_SENDTO, ap_pkt, e, sizeof e, 0, 0, 0);
            }
            thr_sleep_us(ap_assoc == 1 && testing_wpa ? 20000 : ap_assoc ? 1000000 : 20000);
            continue;
        }
        for (long o = 0; o + 20 <= n;) {
            u32 len = get32(b + o);
            if (len < 20 || o + len > (u32)n) break;
            attrs_t at;
            parse(&at, b + o + 20, (int)len - 20);
            if (b[o + 16] == 59 && at.p[51] && at.l[51] >= 24) {          /* a management frame for the AP */
                const u8 *f = at.p[51];
                u8 r[64];
                memset(r, 0, sizeof r);
                memcpy(r + 4, f + 10, 6); memcpy(r + 10, ap_mac, 6); memcpy(r + 16, ap_mac, 6);
                if (f[0] == 0xb0) {                                      /* authentication: open system, accepted */
                    r[0] = 0xb0; r[26] = 2;
                    LOG("test AP: authentication from the client");
                    ap_frame_out(r, 30);
                } else if (f[0] == 0x00) {                               /* association: the station, then the answer */
                    static const u8 rates[4] = { 0x82, 0x84, 0x8b, 0x96 };
                    u8 fl[8];
                    u32 set = (1u << 5) | (1u << 7) | (testing_wpa ? 0 : 1u << 1);   /* WPA2: authorized after the handshake */
                    put32(fl, (1u << 1) | (1u << 5) | (1u << 7)); put32(fl + 4, set);
                    msg((u16)family, 4, C_NEW_STATION);
                    attr32(A_IFINDEX, (u32)ap_if);
                    attr(A_MAC, f + 10, 6);
                    attr16(16, 1);                                          /* STA_AID */
                    attr16(18, 10);                                         /* STA_LISTEN_INTERVAL */
                    attr(19, rates, 4);                                     /* STA_SUPPORTED_RATES */
                    attr(A_STA_FLAGS2, fl, 8);
                    int rr = nl_call_on(ap_sock, NULL, NULL);
                    r[0] = 0x10; r[24] = 1; r[28] = 1; r[29] = 0xc0;      /* ESS; status 0; AID 1 */
                    r[30] = 1; r[31] = 4; memcpy(r + 32, rates, 4);
                    LOG("test AP: association from the client (station: %d)", rr);
                    ap_frame_out(r, 36);
                    ap_assoc = 1;
                    if (testing_wpa) {                                     /* message 1 of 4 */
                        memcpy(sta_mac, f + 10, 6);
                        random_bytes(ap_anonce, 32);
                        thr_sleep_us(100000);
                        ap_eapol(2 | 8 | 0x80, 1, NULL, 0);
                        LOG("test AP: message 1 sent");
                    }
                }
            }
            o += (len + 3) & ~3u;
        }
    }
}
static void test_ap_setup(void) {
    if (!rd_u32("/sys/class/net/wlan1/ifindex")) { LOG("test AP: no wlan1"); return; }
    ap_if = (int)rd_u32("/sys/class/net/wlan1/ifindex");
    char t[32];
    if (rd("/sys/class/net/wlan1/address", t, sizeof t) >= 17)
        for (int i = 0; i < 6; i++) {
            int v = 0;
            for (int j = 0; j < 2; j++) { char c = t[i * 3 + j]; v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0'); }
            ap_mac[i] = (u8)v;
        }
    msg((u16)family, 4, 6);                                            /* SET_INTERFACE: an access point */
    attr32(A_IFINDEX, (u32)ap_if);
    attr32(5, 3);
    int r1 = nl_call(NULL, NULL);
    int r2 = if_up_name("wlan1", 1);
    ap_sock = nl_open();
    static const u16 types[2] = { 0x00b0, 0x0000 };
    for (int i = 0; i < 2; i++) {
        msg((u16)family, 4, 58);                                       /* REGISTER_FRAME */
        attr32(A_IFINDEX, (u32)ap_if);
        attr16(101, types[i]);
        attr(91, NULL, 0);
        nl_call_on(ap_sock, NULL, NULL);
    }
    u8 head[64];
    int h = 0;
    memset(head, 0, sizeof head);
    head[0] = 0x80; memset(head + 4, 0xff, 6); memcpy(head + 10, ap_mac, 6); memcpy(head + 16, ap_mac, 6);
    h = 24 + 8;
    head[h++] = 100; head[h++] = 0;                                     /* beacon interval */
    head[h++] = testing_wpa ? 0x11 : 1; head[h++] = 0;                  /* ESS (+ privacy) */
    head[h++] = 0; head[h++] = 8; memcpy(head + h, "QRT-Test", 8); h += 8;
    head[h++] = 1; head[h++] = 4; head[h++] = 0x82; head[h++] = 0x84; head[h++] = 0x8b; head[h++] = 0x96;
    head[h++] = 3; head[h++] = 1; head[h++] = 1;                        /* channel 1 */
    msg((u16)family, 4, 15);                                           /* START_AP */
    attr32(A_IFINDEX, (u32)ap_if);
    attr(14, head, h);
    attr(15, testing_wpa ? rsn_ie : NULL, testing_wpa ? 22 : 0);        /* the tail: the RSN element */
    if (testing_wpa) {
        attr(A_PRIVACY, NULL, 0);
        attr32(A_WPA_VERSIONS, 2);
        attr32(A_CIPHERS_PAIRWISE, CIPHER_CCMP);
        attr32(A_CIPHER_GROUP, CIPHER_CCMP);
        attr32(A_AKM_SUITES, AKM_PSK);
        attr(A_CONTROL_PORT, NULL, 0);
        attr16(A_CONTROL_PORT_ETHERTYPE, 0x888e);
        pbkdf2_sha1("qrtpassword", (const u8 *)"QRT-Test", 8, 4096, ap_pmk, 32);
        random_bytes(ap_gtk, 16);
    }
    attr32(12, 100);
    attr32(13, 1);
    attr(A_SSID, "QRT-Test", 8);
    attr32(126, 0);
    attr32(A_WIPHY_FREQ, 2412);
    attr32(159, 0);
    attr32(A_AUTH_TYPE, 0);
    int r3 = nl_call(NULL, NULL);
    ap_pkt = (int)S(NR_SOCKET, 17, 3, 0x0300, 0, 0);
    u8 sll[20];
    memset(sll, 0, sizeof sll);
    put16(sll, 17); sll[3] = 0x03; put32(sll + 4, (u32)ap_if);
    S(NR_BIND, ap_pkt, sll, 20, 0, 0);
    LOG("test AP: \"QRT-Test\" on wlan1 (access point mode %d, up %d, started %d)", r1, r2, r3);
    thr_create("wifi test ap", ap_loop, NULL, 32 << 10);
}

/* starting takes long on the phone (the modem partition mounted, the Pronto core
 * started, wcn36xx up): a thread does it, wlan.c asks again until it is done */
static volatile int start_state;                      /* 0 idle, 1 running, 2 done, -1 failed */
static void start_work(void *a) {
    (void)a;
    int ok = 0;
    S(NR_MKDIRAT, AT_FDCWD, "/proc", 0555, 0, 0);
    S(40, "proc", "/proc", "proc", 0, 0);                              /* mount: /proc/sys for wlan0's settings */
    if (nl80211_open() == 0) {
        if (testing) test_ap_setup();
        if (!find_wlan()) {
            LOG("starting the Wi-Fi core");
            if (start_pronto() == 0)
                for (int t = 0; t < 100 && !find_wlan(); t++) thr_sleep_us(200000);   /* wcn36xx comes up over SMD */
            if (!ifindex) LOG("no Wi-Fi interface appeared in Linux");
        }
        if (ifindex) {
            char p[96];
            fmt(p, sizeof p, "/proc/sys/net/ipv6/conf/%s/disable_ipv6", ifname);   /* IP is Tessera's */
            wr(p, "1");
            /* rfkill: unblocked (a soft block refuses the interface) */
            for (int i = 0; i < 4; i++) { char rp[64]; fmt(rp, sizeof rp, "/sys/class/rfkill/rfkill%d/soft", i); wr(rp, "0"); }
            int r = -1;
            for (int t = 0; t < 6 && (r = if_up(1)) != 0; t++) {        /* the core may still be booting */
                LOG("%s would not come up (error %d)%s", ifname, r, t < 5 ? ", trying again" : "");
                if (t < 5) thr_sleep_us(2000000);
            }
            if (r) {
                void argon_wifi_report(void);
                argon_wifi_report();                                       /* wcn36xx's own words */
            } else if (pkt_open() == 0) {
                ok = 1;
                LOG("%s up, address %02x:%02x:%02x:%02x:%02x:%02x", ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
        }
    }
    up = ok;
    start_state = ok ? 2 : -1;
}
static int lw_start(void) {
    if (up) return 0;
    if (start_state == 1) return 1;
    if (start_state == -1) { start_state = 0; return -1; }
    start_state = 1;
    thr_create("wifi start", start_work, NULL, 64 << 10);
    return 1;
}
static void lw_stop(void) { if (up) { if_up(0); up = 0; scanning = 0; } }
static int lw_scan(void) {
    if (!up) return -1;
    msg((u16)family, 4, C_TRIGGER_SCAN);                                /* NLM_F_ACK */
    attr32(A_IFINDEX, (u32)ifindex);
    nest_begin(A_SCAN_SSIDS); attr(1, "", 0); nest_end();               /* the wildcard SSID */
    int r = nl_call(NULL, NULL);
    if (r < 0 && r != -16) { LOG("scan refused (%d)", r); return -1; }    /* -EBUSY: one is running */
    scanning = 1;
    return 0;
}
static int lw_scanning(void) { return scanning; }

static int bss_cb(const u8 *m, int len, void *ctx) {
    (void)ctx;
    attrs_t a, b;
    parse(&a, m + 20, len - 20);
    if (!a.p[A_BSS]) return 0;
    parse(&b, a.p[A_BSS], a.l[A_BSS]);
    if (!b.p[B_BSSID]) return 0;
    const u8 *ies = b.p[B_IES] ? b.p[B_IES] : b.p[B_BEACON_IES];
    int il = b.p[B_IES] ? b.l[B_IES] : b.l[B_BEACON_IES];
    int sig = b.p[B_SIGNAL_MBM] ? (int)(i32)get32(b.p[B_SIGNAL_MBM]) / 100 : -90;
    wlan_sm_bss(b.p[B_BSSID], b.p[B_FREQUENCY] ? (int)get32(b.p[B_FREQUENCY]) : 2412, sig,
                b.p[B_CAPABILITY] ? get16(b.p[B_CAPABILITY]) : 0, b.p[B_BEACON_INTERVAL] ? get16(b.p[B_BEACON_INTERVAL]) : 100,
                ies ? ies : (const u8 *)"", ies ? (usize)il : 0);
    return 0;
}
static void scan_results(void) {
    msg((u16)family, 0x300, C_GET_SCAN);                                /* NLM_F_DUMP */
    attr32(A_IFINDEX, (u32)ifindex);
    nl_call(bss_cb, NULL);
}

static u8 cur_bssid[6];
static int lw_connect(const u8 bssid[6], const char *ssid, int freq, const u8 *ie, usize ielen) {
    if (!up) return -1;
    memcpy(cur_bssid, bssid, 6);
    msg((u16)family, 4, C_CONNECT);
    attr32(A_IFINDEX, (u32)ifindex);
    attr(A_MAC, bssid, 6);
    attr(A_SSID, ssid, (int)strlen(ssid));
    attr32(A_WIPHY_FREQ, (u32)freq);
    attr32(A_AUTH_TYPE, 0);                                             /* open system */
    if (ie && ielen >= 8) {
        attr(A_IE, ie, (int)ielen);
        attr(A_PRIVACY, NULL, 0);
        attr32(A_WPA_VERSIONS, 2);
        attr32(A_CIPHERS_PAIRWISE, CIPHER_CCMP);
        attr32(A_CIPHER_GROUP, 0x000fac00u | ie[7]);                    /* the AP's group cipher */
        attr32(A_AKM_SUITES, AKM_PSK);
        attr(A_CONTROL_PORT, NULL, 0);                                  /* data waits for the handshake */
        attr16(A_CONTROL_PORT_ETHERTYPE, 0x888e);
    }
    int r = nl_call(NULL, NULL);
    if (r < 0) { LOG("connect refused (%d)", r); return -1; }
    return 0;
}
static void lw_disconnect(void) {
    if (!up) return;
    msg((u16)family, 4, C_DISCONNECT);
    attr32(A_IFINDEX, (u32)ifindex);
    attr16(A_REASON_CODE, 3);
    nl_call(NULL, NULL);
}
static int lw_send(const u8 *eth, usize len) {
    if (pkt < 0) return -1;
    return S6(NR_SENDTO, pkt, eth, len, 0, 0, 0) == (long)len ? 0 : -1;
}
static int lw_set_keys(const u8 tk[16], int gtk_id, const u8 gtk[16]) {
    int r = 0;
    if (tk) {
        msg((u16)family, 4, C_NEW_KEY);
        attr32(A_IFINDEX, (u32)ifindex);
        attr(A_MAC, cur_bssid, 6);
        attr8(A_KEY_IDX, 0);
        attr(A_KEY_DATA, tk, 16);
        attr32(A_KEY_CIPHER, CIPHER_CCMP);
        if ((r = nl_call(NULL, NULL)) < 0) { LOG("pairwise key refused (%d)", r); return -1; }
    }
    if (gtk) {
        u8 rsc[6] = { 0 };
        msg((u16)family, 4, C_NEW_KEY);
        attr32(A_IFINDEX, (u32)ifindex);
        attr8(A_KEY_IDX, (u8)gtk_id);
        attr(A_KEY_DATA, gtk, 16);
        attr32(A_KEY_CIPHER, CIPHER_CCMP);
        attr(A_KEY_SEQ, rsc, 6);
        attr32(A_KEY_TYPE, 0);                                          /* group */
        if ((r = nl_call(NULL, NULL)) < 0) LOG("group key refused (%d)", r);
    }
    if (tk) {                                                         /* the port opens: data flows */
        u8 fl[8];
        put32(fl, 1u << 1); put32(fl + 4, 1u << 1);                     /* mask, set: AUTHORIZED */
        msg((u16)family, 4, C_SET_STATION);
        attr32(A_IFINDEX, (u32)ifindex);
        attr(A_MAC, cur_bssid, 6);
        attr(A_STA_FLAGS2, fl, 8);
        if ((r = nl_call(NULL, NULL)) < 0) { LOG("could not open the port (%d)", r); return -1; }
    }
    return 0;
}
static const u8 *lw_mac(void) { return mac; }

/* events (scan done, connected, disconnected) and received frames */
static void lw_poll(void) {
    if (!up) return;
    for (int i = 0; i < 8; i++) {
        long n = S6(NR_RECVFROM, nle, rb, sizeof rb, 0x40, 0, 0);       /* MSG_DONTWAIT */
        if (n <= 0) break;
        for (long o = 0; o + 20 <= n;) {
            u32 len = get32(rb + o);
            if (len < 20 || o + len > (u32)n) break;
            u8 cmd = rb[o + 16];
            attrs_t a;
            parse(&a, rb + o + 20, (int)len - 20);
            int mine = !a.p[A_IFINDEX] || (int)get32(a.p[A_IFINDEX]) == ifindex;
            if (mine && cmd == C_NEW_SCAN_RESULTS) { scan_results(); scanning = 0; }
            else if (mine && cmd == C_SCAN_ABORTED) { LOG("scan aborted"); scanning = 0; }
            else if (mine && cmd == C_CONNECT) {
                int st = a.p[A_STATUS_CODE] ? get16(a.p[A_STATUS_CODE]) : (a.p[65] ? -1 : 0);   /* TIMED_OUT */
                LOG("connect: status %d", st);
                wlan_sm_assoc(st == 0, st);
            } else if (mine && cmd == C_DISCONNECT) {
                int rc = a.p[A_REASON_CODE] ? get16(a.p[A_REASON_CODE]) : 0;
                LOG("disconnected (reason %d)", rc);
                wlan_sm_lost(rc);
            }
            o += (len + 3) & ~3u;
        }
    }
    while (rq_tail != rq_head) {
        const u8 *f = rxq[rq_tail % RXQ].f;
        static int seen;
        if (testing && f[12] == 0x88 && f[13] == 0xb5 && seen++ < 3) LOG("test: a frame from the access point arrived");
        wlan_sm_eth(rxq[rq_tail % RXQ].f, rxq[rq_tail % RXQ].len);
        rq_tail++;
    }
}

static const wlan_softmac_t lw = { lw_present, lw_start, lw_stop, lw_scan, lw_scanning, lw_connect, lw_disconnect,
                                   lw_send, lw_set_keys, lw_mac, lw_poll };
const wlan_softmac_t *wlan_softmac(void) { return &lw; }
/* qrt.wifitest (QEMU): Wi-Fi on, the test network saved - the shell connects at boot */
void lwifi_test_boot(int wpa) {
    testing = 1;
    testing_wpa = wpa;
    hal_setting_set(u"QrtWifiOn", 1);
    hal_setting_set_blob(u"QrtWifiSsid", "QRT-Test", 8);
    if (wpa) {                                                         /* the password, saved as its key */
        u8 pmk[32];
        pbkdf2_sha1("qrtpassword", (const u8 *)"QRT-Test", 8, 4096, pmk, 32);
        hal_setting_set_blob(u"QrtWifiPmk", pmk, 32);
    } else hal_setting_set_blob(u"QrtWifiPmk", NULL, 0);
}
#endif
