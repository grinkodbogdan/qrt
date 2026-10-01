/* Host test for src/drivers/bt/hci.c: a simulated Intel Wireless 8260
 * Bluetooth controller.  It starts in the bootloader (Read Version says
 * firmware variant 0x06), takes the real ibt-11-5.sfi through Secure Send
 * and checks every fragment the way the bootloader would: CSS header, public
 * key and signature as their own fragment types, then command-stream
 * fragments that end on whole commands at a 4-byte boundary and fit 252
 * bytes, all of it adding up to the file.  It then boots (Intel Reset, then
 * the bootup vendor event), takes the DDC entries, and answers a scan with an
 * extended inquiry result, an inquiry result with RSSI (named later by Remote
 * Name Request) and an LE advertising report.
 * Build/run: make check */
#include <stdio.h>
#include <stdlib.h>
#include "../src/drivers/bt/hci.c"

/* ---- the kernel environment hci.c expects -------------------------------------- */
kernel_t k;
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
void klog(const char *f, ...) { va_list ap; va_start(ap, f); if (getenv("VERBOSE")) { vprintf(f, ap); printf("\n"); } va_end(ap); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the simulated controller ----------------------------------------------------- */
static u8 *fw; static long fwlen;
static u8 *boundary;               /* boundary[i]: a command ends at stream offset i */
static struct {
    int operational;
    int got_header, got_pkey, got_sign;
    long data_bytes;               /* command stream received so far */
    int frags, bad_frag;
    int ddc_cmds;
    int reset_seen;
    int via_bulk, via_ctrl_fc09;
    int remote_name_asked;
} D;

static void event(const u8 *e, int len) { bt_rx_event(e, len); }
static void cc(u16 op, const u8 *ret, int rl) {
    u8 e[260] = { 0x0e, (u8)(3 + rl), 1, (u8)op, (u8)(op >> 8) };
    memcpy(e + 5, ret, (usize)rl);
    event(e, 5 + rl);
}
static void cs(u16 op, u8 status) { u8 e[6] = { 0x0f, 4, status, 1, (u8)op, (u8)(op >> 8) }; event(e, 6); }

static void secure_send_frag(const u8 *p, int plen) {
    u8 type = p[0];
    const u8 *d = p + 1;
    int n = plen - 1;
    D.frags++;
    if (n > 252) D.bad_frag++;
    if (type == 0x00) { CHECK(n == 128 && !memcmp(d, fw, 128), "CSS header fragment"); D.got_header = 1; }
    else if (type == 0x03) { CHECK(D.got_header && !memcmp(d, fw + 128 + (D.got_pkey ? 252 : 0), (usize)n), "public key bytes"); D.got_pkey += n; }
    else if (type == 0x02) { CHECK(D.got_pkey == 256 && !memcmp(d, fw + 388 + (D.got_sign ? 252 : 0), (usize)n), "signature bytes"); D.got_sign += n; }
    else if (type == 0x01) {
        CHECK(D.got_sign == 256, "data before the signature");
        if (memcmp(d, fw + 644 + D.data_bytes, (usize)n)) { if (!D.bad_frag) printf("  data fragment %d differs at %ld\n", D.frags, D.data_bytes); D.bad_frag++; }
        D.data_bytes += n;
        if (n < 252 && (!boundary[D.data_bytes] || D.data_bytes % 4)) { if (!D.bad_frag) printf("  fragment %d ends mid-block at %ld\n", D.frags, D.data_bytes); D.bad_frag++; }
    } else D.bad_frag++;
    u8 ok = 0;
    cc(0xfc09, &ok, 1);
    if (type == 0x01 && 644 + D.data_bytes == fwlen) {       /* everything arrived: download result */
        static const u8 done[] = { 0xff, 2, 0x06, 0x00 };
        event(done, sizeof done);
    }
}

/* Linux's rule, seen from the receiving side: a data Secure Send is either a
 * full 252-byte piece of a larger block, or the end of a block - and blocks
 * end on a whole command at a multiple of 4 bytes. */
static void mark_boundaries(void) {
    long len = fwlen - 644;
    boundary = calloc((usize)len + 1, 1);
    for (long o = 0; o + 3 <= len; ) { o += 3 + fw[644 + o + 2]; if (o <= len) boundary[o] = 1; }
}

static int send_bulk(const u8 *p, int len) {
    D.via_bulk++;
    u16 op = (u16)(p[0] | p[1] << 8);
    CHECK(op == 0xfc09 && p[2] == len - 3, "bulk OUT carries only Secure Send (got %04x)", op);
    secure_send_frag(p + 3, p[2]);
    return 0;
}

static int send_cmd(const u8 *p, int len) {
    u16 op = (u16)(p[0] | p[1] << 8);
    const u8 *a = p + 3;
    int plen = p[2];
    CHECK(plen == len - 3, "command %04x length", op);
    u8 ok = 0;
    switch (op) {
    case 0xfc05: {
        u8 v[10] = { 0, 0x37, 0x0b, 0x10, (u8)(D.operational ? 0x23 : 0x06), 0x10, 37, 52, 15, 0 };
        cc(op, v, 10);
        break;
    }
    case 0xfc0d: {
        u8 bp[23] = { 0 };
        bp[4] = 5; bp[6] = 1;                                   /* revision 5, secure boot */
        bp[12] = 0x11; bp[13] = 0x22;                           /* an OTP address */
        cc(op, bp, 23);
        break;
    }
    case 0xfc09: D.via_ctrl_fc09++; secure_send_frag(a, plen); break;
    case 0xfc01: {
        CHECK(plen == 8 && a[5] == 0x08 && a[6] == 0x04, "Intel Reset parameters");
        D.reset_seen = 1;
        D.operational = 1;
        static const u8 bootup[] = { 0xff, 7, 0x02, 0, 1, 0, 0, 0, 0 };
        event(bootup, sizeof bootup);                            /* no command complete: Linux injects one */
        break;
    }
    case 0xfc8b: D.ddc_cmds++; CHECK(a[0] + 1 == plen, "DDC entry length"); cc(op, &ok, 1); break;
    case 0x1009: { u8 r[7] = { 0, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 }; cc(op, r, 7); break; }
    case 0x0401: {                                              /* Inquiry */
        cs(op, 0);
        u8 eir[2 + 255] = { 0x2f, 255, 1, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 1, 0, 0x0c, 0x02, 0x5a, 0, 0, (u8)-48 };
        const char *nm = "Pixel 7";
        eir[17] = (u8)(strlen(nm) + 1); eir[18] = 0x09; memcpy(eir + 19, nm, strlen(nm));
        event(eir, 2 + 255);
        u8 r[2 + 15] = { 0x22, 15, 1, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 1, 0, 0x04, 0x04, 0x24, 0, 0, (u8)-70 };
        event(r, sizeof r);
        u8 done[3] = { 0x01, 1, 0 };
        event(done, 3);
        break;
    }
    case 0x200c:
        cc(op, &ok, 1);
        if (a[0]) {                                             /* LE scan on: one advertising report */
            const char *nm = "Mi Band";
            int dl = 2 + (int)strlen(nm);
            u8 e[64] = { 0x3e, 0, 0x02, 1, 0x00, 0x01, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, (u8)dl, (u8)(dl - 1), 0x09 };
            memcpy(e + 15, nm, strlen(nm));
            e[13 + dl] = (u8)-60;
            e[1] = (u8)(12 + dl);
            event(e, 14 + dl);
        }
        break;
    case 0x0419: {                                              /* Remote Name Request */
        D.remote_name_asked++;
        cs(op, 0);
        u8 e[2 + 255] = { 0x07, 255, 0 };
        memcpy(e + 3, a, 6);
        memcpy(e + 9, "Car Kit", 8);
        event(e, 2 + 255);
        break;
    }
    default: cc(op, &ok, 1); break;
    }
    return 0;
}

static void poll(void) {}
static void sleep_ms(u32 ms) { (void)ms; }
static u8 *read_file(const char *path, u64 *len) {
    char p[256];
    const char *base = strrchr(path, '/');
    snprintf(p, sizeof p, "firmware/%s", base ? base + 1 : path);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = malloc((usize)n);
    if (fread(b, 1, (usize)n, f) != (usize)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (u64)n;
    return b;
}
static void free_file(u8 *p) { free(p); }

int main(void) {
    u64 n;
    fw = read_file("/lib/firmware/ibt-11-5.sfi", &n);
    if (!fw) { printf("test_bt: firmware/ibt-11-5.sfi missing\n"); return 1; }
    fwlen = (long)n;
    mark_boundaries();
    static const bt_transport_t t = { send_cmd, send_bulk, poll, sleep_ms, read_file, free_file };
    bt_attach(&t, 0x8087, 0x0a2b);
    CHECK(bt_state() == BT_READY, "controller not ready: %s", bt_status());
    CHECK(D.got_header && D.got_pkey == 256 && D.got_sign == 256, "header/key/signature");
    CHECK(644 + D.data_bytes == fwlen, "firmware bytes sent %ld of %ld", 644 + D.data_bytes, fwlen - 644 + 644);
    CHECK(!D.bad_frag, "%d bad fragments", D.bad_frag);
    CHECK(D.via_bulk == D.frags && !D.via_ctrl_fc09, "Secure Send must go down bulk OUT in the bootloader");
    CHECK(D.reset_seen && D.ddc_cmds == 4, "reset %d, DDC commands %d (ibt-11-5.ddc has 4 entries)", D.reset_seen, D.ddc_cmds);
    CHECK(strstr(bt_status(), "11:22:33:44:55:66") != NULL, "address: %s", bt_status());
    printf("  firmware: %d Secure Send fragments, %ld bytes, booted, %d DDC entries\n", D.frags, 644 + D.data_bytes, D.ddc_cmds);

    scan();
    bt_device_t dv[8];
    int nd = bt_devices(dv, 8);
    CHECK(nd == 3, "devices found: %d", nd);
    int pixel = 0, car = 0, band = 0;
    for (int i = 0; i < nd; i++) {
        if (!strcmp(dv[i].name, "Pixel 7") && dv[i].rssi == -48 && !strcmp(bt_kind(dv[i].cod, dv[i].le), "Phone")) pixel = 1;
        if (!strcmp(dv[i].name, "Car Kit") && dv[i].rssi == -70 && !strcmp(bt_kind(dv[i].cod, dv[i].le), "Headset")) car = 1;
        if (!strcmp(dv[i].name, "Mi Band") && dv[i].le && dv[i].rssi == -60) band = 1;
        printf("  found: %-8s %-20s %d dBm\n", dv[i].name, bt_kind(dv[i].cod, dv[i].le), dv[i].rssi);
    }
    CHECK(pixel && car && band, "scan results: pixel %d car %d band %d", pixel, car, band);
    CHECK(D.remote_name_asked == 1, "remote name requests: %d", D.remote_name_asked);
    if (fails) { printf("test_bt: %d failures\n", fails); return 1; }
    printf("test_bt: Intel firmware download, boot and scan pass\n");
    return 0;
}
