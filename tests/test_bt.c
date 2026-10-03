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
#include <math.h>
#include "../src/drivers/bt/hci.c"
#include "../src/drivers/bt/l2cap.c"
#include "../src/drivers/bt/a2dp.c"
#include "../src/drivers/bt/sbc.c"

/* ---- the kernel environment hci.c expects -------------------------------------- */
kernel_t k;
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
void klog(const char *f, ...) { va_list ap; va_start(ap, f); if (getenv("VERBOSE")) { vprintf(f, ap); printf("\n"); } va_end(ap); }

/* NVRAM (link keys) */
static u8 nv_keys[256]; static usize nv_len;
usize hal_setting_get_blob(const c16 *name, void *buf, usize cap) { (void)name; if (!nv_len || nv_len > cap) return 0; memcpy(buf, nv_keys, nv_len); return nv_len; }
void hal_setting_set_blob(const c16 *name, const void *buf, usize len) { (void)name; if (len <= sizeof nv_keys) { memcpy(nv_keys, buf, len); nv_len = len; } }

/* the sound core: a 1 kHz tone, and the output A2DP registers */
static snd_output_t *sound_out;
static double tone_phase;
void snd_output_add(snd_output_t *o) { sound_out = o; }
void snd_output_remove(snd_output_t *o) { if (sound_out == o) sound_out = NULL; }
void snd_mix(i16 *out, int frames, int rate, int channels) {
    for (int i = 0; i < frames; i++) {
        i16 v = (i16)(12000 * sin(tone_phase));
        tone_phase += 2 * M_PI * 1000 / rate;
        for (int c = 0; c < channels; c++) out[i * channels + c] = v;
    }
}
static u64 sim_ms;
static u64 now_ms(void) { return sim_ms; }

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

/* ---- the simulated headphones (after the controller has booted) -------------------------------- */
static const u8 HS[6] = { 0x01, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0 };
static const u8 HS_KEY[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
#define HS_HANDLE 0x0042
static struct {
    int linked, encrypted, paired_ssp, key_replies, key_ok;
    struct { u16 our, theirs, psm; int ours_ok, theirs_ok; } ch[6];
    int nch;
    u16 next_cid;
    int sdp_asked, sdp_ok;
    int discovered, caps_aac, caps_sbc, configured, opened, started, suspended;
    u8 conf[4];
    FILE *sbc;
    int packets, frames, seq_errs, bad_rtp;
    u16 last_seq;
    int disconnected;
} H;

static void hs_acl(const u8 *l2, int len) {
    u8 pkt[1100];
    pkt[0] = (u8)HS_HANDLE; pkt[1] = (u8)((HS_HANDLE >> 8) | 0x20); pkt[2] = (u8)len; pkt[3] = (u8)(len >> 8);
    memcpy(pkt + 4, l2, (usize)len);
    bt_rx_acl(pkt, 4 + len);
}
static void hs_send(u16 cid, const u8 *p, int len) {
    u8 f[1100];
    f[0] = (u8)len; f[1] = (u8)(len >> 8); f[2] = (u8)cid; f[3] = (u8)(cid >> 8);
    memcpy(f + 4, p, (usize)len);
    hs_acl(f, 4 + len);
}
static void hs_sig(u8 code, u8 ident, const u8 *d, int len) {
    u8 c[64] = { code, ident, (u8)len, 0 };
    memcpy(c + 4, d, (usize)len);
    hs_send(0x0001, c, 4 + len);
}
static int hs_ch_by_ours(u16 cid) { for (int i = 0; i < H.nch; i++) if (H.ch[i].our == cid) return i; return -1; }
static void hs_config_req(int i) { u8 d[8] = { (u8)H.ch[i].theirs, (u8)(H.ch[i].theirs >> 8), 0, 0, 0x01, 2, 0x7f, 0x03 }; hs_sig(0x04, 0x40 + (u8)i, d, 8); }   /* MTU 895 */

/* the headphones ask our SDP server what we are */
static void hs_sdp_query(void) {
    int i = H.nch++;
    H.ch[i].our = H.next_cid++; H.ch[i].psm = 1;
    u8 d[4] = { 0x01, 0x00, (u8)H.ch[i].our, (u8)(H.ch[i].our >> 8) };
    hs_sig(0x02, 0x30, d, 4);
}

static void hs_avdtp(int i, const u8 *p, int len) {
    u8 label = p[0] >> 4, sig = p[1] & 0x3f;
    u8 r[64] = { (u8)(label << 4 | 2), sig };
    int rl = 2;
    switch (sig) {
    case 0x01: H.discovered++; r[2] = 1 << 2; r[3] = 0x08; r[4] = 2 << 2; r[5] = 0x08; rl = 6; break;      /* two audio sinks */
    case 0x02:
        if ((p[2] >> 2) == 1) { static const u8 aac[] = { 0x01, 0x00, 0x07, 0x08, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0xe2, 0x00 }; memcpy(r + 2, aac, sizeof aac); rl = 2 + (int)sizeof aac; H.caps_aac++; }
        else { static const u8 sbc2[] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xff, 0xff, 2, 53 }; memcpy(r + 2, sbc2, sizeof sbc2); rl = 2 + (int)sizeof sbc2; H.caps_sbc++; }
        break;
    case 0x03: CHECK(len >= 12 && (p[2] >> 2) == 2, "SET_CONFIGURATION for the SBC sink"); if (len >= 12) memcpy(H.conf, p + 10, 4); H.configured++; break;
    case 0x06: H.opened++; break;
    case 0x07: H.started++; break;
    case 0x09: H.suspended++; break;
    }
    hs_send(H.ch[i].theirs, r, rl);
}

static void hs_media(const u8 *p, int len) {
    if (len < 13 || p[0] != 0x80 || (p[1] & 0x7f) != 0x60) { H.bad_rtp++; return; }
    u16 seq = (u16)(p[2] << 8 | p[3]);
    if (H.packets && seq != (u16)(H.last_seq + 1)) H.seq_errs++;
    H.last_seq = seq;
    H.packets++;
    int n = p[12] & 15;
    H.frames += n;
    if (H.sbc) fwrite(p + 13, 1, (size_t)(len - 13), H.sbc);
}

static void hs_l2cap(const u8 *f, int len) {
    int l = f[0] | f[1] << 8;
    u16 cid = (u16)(f[2] | f[3] << 8);
    const u8 *p = f + 4;
    CHECK(4 + l == len, "L2CAP length %d vs %d", 4 + l, len);
    if (cid == 1) {
        u8 code = p[0], id = p[1];
        const u8 *d = p + 4;
        if (code == 0x02) {                                    /* Connection Request from QRT */
            int i = H.nch++;
            H.ch[i].psm = (u16)(d[0] | d[1] << 8); H.ch[i].theirs = (u16)(d[2] | d[3] << 8); H.ch[i].our = H.next_cid++;
            u8 r[8] = { (u8)H.ch[i].our, (u8)(H.ch[i].our >> 8), d[2], d[3], 0, 0, 0, 0 };
            hs_sig(0x03, id, r, 8);
            hs_config_req(i);
        } else if (code == 0x03) {                             /* Connection Response (our SDP channel) */
            int i = hs_ch_by_ours((u16)(d[2] | d[3] << 8));
            CHECK(i >= 0 && !(d[4] | d[5]), "SDP channel accepted");
            if (i >= 0) { H.ch[i].theirs = (u16)(d[0] | d[1] << 8); hs_config_req(i); }
        } else if (code == 0x04) {                             /* Configuration Request */
            int i = hs_ch_by_ours((u16)(d[0] | d[1] << 8));
            CHECK(i >= 0, "config for an unknown channel");
            if (i < 0) return;
            u8 r[6] = { (u8)H.ch[i].theirs, (u8)(H.ch[i].theirs >> 8), 0, 0, 0, 0 };
            hs_sig(0x05, id, r, 6);
            H.ch[i].theirs_ok = 1;
        } else if (code == 0x05) {                             /* Configuration Response */
            int i = hs_ch_by_ours((u16)(d[0] | d[1] << 8));
            if (i >= 0) H.ch[i].ours_ok = 1;
            if (i >= 0 && H.ch[i].psm == 1 && H.ch[i].theirs_ok) {     /* open: ask */
                static const u8 q[] = { 0x06, 0x00, 0x01, 0x00, 0x0f, 0x35, 0x03, 0x19, 0x11, 0x0a, 0xff, 0xff, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00 };
                hs_send(H.ch[i].theirs, q, sizeof q);
                H.sdp_asked++;
            }
        } else if (code == 0x06) {                             /* Disconnection Request */
            hs_sig(0x07, id, d, 4);
        }
        return;
    }
    int i = hs_ch_by_ours(cid);
    CHECK(i >= 0, "data on an unknown channel %04x", cid);
    if (i < 0) return;
    if (H.ch[i].psm == 1) {                                    /* SDP answer: our A2DP source record */
        H.sdp_ok = p[0] == 0x07 && l > 20;
        for (int k = 0; k + 2 < l; k++) if (p[k] == 0x19 && p[k + 1] == 0x11 && p[k + 2] == 0x0a) H.sdp_ok = 2;
        return;
    }
    int first25 = 1;
    for (int k = 0; k < i; k++) if (H.ch[k].psm == 25) first25 = 0;
    if (first25) hs_avdtp(i, p, l);
    else hs_media(p, l);
}

static void hs_command(u16 op, const u8 *a) {
    u8 ok = 0;
    switch (op) {
    case 0x0405: {                                             /* Create Connection */
        CHECK(!memcmp(a, HS, 6), "Create Connection to the headphones");
        cs(op, 0);
        u8 e[13] = { 0x03, 11, 0, (u8)HS_HANDLE, HS_HANDLE >> 8 };
        memcpy(e + 5, HS, 6); e[11] = 1; e[12] = 0;
        event(e, 13);
        H.linked = 1;
        break;
    }
    case 0x0411: {                                             /* Authentication Requested */
        cs(op, 0);
        u8 e[8] = { 0x17, 6 }; memcpy(e + 2, HS, 6); event(e, 8);     /* Link Key Request */
        break;
    }
    case 0x040b: {                                             /* Link Key Request Reply */
        H.key_replies++;
        H.key_ok = !memcmp(a + 6, HS_KEY, 16);
        u8 r[7] = { 0 }; memcpy(r + 1, HS, 6); cc(op, r, 7);
        u8 e[5] = { 0x06, 3, 0, (u8)HS_HANDLE, HS_HANDLE >> 8 }; event(e, 5);   /* Authentication Complete */
        break;
    }
    case 0x040c: {                                             /* Negative Reply: pair */
        u8 r[7] = { 0 }; memcpy(r + 1, HS, 6); cc(op, r, 7);
        u8 e[8] = { 0x31, 6 }; memcpy(e + 2, HS, 6); event(e, 8);     /* IO Capability Request */
        break;
    }
    case 0x042b: {                                             /* IO Capability Request Reply */
        CHECK(a[6] == 0x03 && a[8] == 0x04, "IO capability NoInputNoOutput, general bonding");
        u8 r[7] = { 0 }; memcpy(r + 1, HS, 6); cc(op, r, 7);
        u8 e[11] = { 0x32, 9 }; memcpy(e + 2, HS, 6); e[8] = 0x03; e[9] = 0; e[10] = 0x00; event(e, 11);
        u8 u[12] = { 0x33, 10 }; memcpy(u + 2, HS, 6); event(u, 12);  /* User Confirmation Request */
        break;
    }
    case 0x042c: {                                             /* User Confirmation Request Reply */
        u8 r[7] = { 0 }; memcpy(r + 1, HS, 6); cc(op, r, 7);
        u8 e1[9] = { 0x36, 7, 0 }; memcpy(e1 + 3, HS, 6); event(e1, 9);
        u8 e2[25] = { 0x18, 23 }; memcpy(e2 + 2, HS, 6); memcpy(e2 + 8, HS_KEY, 16); e2[24] = 4; event(e2, 25);
        u8 e3[5] = { 0x06, 3, 0, (u8)HS_HANDLE, HS_HANDLE >> 8 }; event(e3, 5);
        H.paired_ssp = 1;
        break;
    }
    case 0x0413: {                                             /* Set Connection Encryption */
        cs(op, 0);
        u8 e[6] = { 0x08, 4, 0, (u8)HS_HANDLE, HS_HANDLE >> 8, 1 }; event(e, 6);
        H.encrypted = 1;
        hs_sdp_query();                                        /* the headphones look us up */
        break;
    }
    case 0x0406: {                                             /* Disconnect */
        cs(op, 0);
        u8 e[6] = { 0x05, 4, 0, (u8)HS_HANDLE, HS_HANDLE >> 8, 0x16 }; event(e, 6);
        H.disconnected++;
        H.nch = 0; H.linked = 0;
        break;
    }
    default: cc(op, &ok, 1); break;
    }
}

static int send_bulk(const u8 *p, int len) {
    if (!bt_in_bootloader() && D.operational) {                /* ACL data to the headphones */
        u16 hf = (u16)(p[0] | p[1] << 8);
        int dl = p[2] | p[3] << 8;
        CHECK((hf & 0x0fff) == HS_HANDLE && 4 + dl == len, "ACL packet to handle %03x, %d bytes", hf & 0x0fff, dl);
        CHECK(((hf >> 12) & 3) == 2, "ACL packets start frames (the media fits the MTU)");
        hs_l2cap(p + 4, dl);
        u8 e[7] = { 0x13, 5, 1, (u8)HS_HANDLE, HS_HANDLE >> 8, 1, 0 };   /* Number Of Completed Packets */
        event(e, 7);
        return 0;
    }
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
    case 0x1005: { u8 r[8] = { 0, 0xfd, 0x03, 0x40, 8, 0, 0, 0 }; cc(op, r, 8); break; }   /* Read Buffer Size: 1021 x 8 */
    case 0x0405: case 0x0411: case 0x040b: case 0x040c: case 0x042b: case 0x042c: case 0x0413: case 0x0406:
        hs_command(op, a);
        break;
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
    static const bt_transport_t t = { send_cmd, send_bulk, poll, sleep_ms, read_file, free_file, now_ms };
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

    /* ---- headphones: pair, connect, stream 2 s, disconnect, reconnect with the stored key ---- */
    H.next_cid = 0x0050;
    H.sbc = fopen("build/test_bt_a2dp.sbc", "wb");
    bt_audio_connect(HS);
    u64 start = 0;
    for (int t = 0; t < 20000; t++) {
        sim_ms++;
        bt_poll_once();
        if (sound_out && sim_ms % 4 == 0) sound_out->pump(sound_out);
        if (H.started && !start) start = sim_ms;
        if (start && sim_ms - start >= 2000) break;
    }
    fclose(H.sbc); H.sbc = NULL;
    CHECK(H.paired_ssp && nv_len && !memcmp(nv_keys + 6, HS_KEY, 16), "paired (SSP) and the key stored");
    CHECK(H.encrypted, "encryption on");
    CHECK(H.sdp_asked && H.sdp_ok == 2, "SDP: our A2DP source record found (%d)", H.sdp_ok);
    CHECK(H.discovered == 1 && H.caps_aac == 1 && H.caps_sbc == 1, "discovery: AAC skipped, SBC found (%d %d %d)", H.discovered, H.caps_aac, H.caps_sbc);
    CHECK(H.configured == 1 && H.conf[0] == 0x22 && H.conf[1] == 0x15 && H.conf[3] == 53, "configuration %02x %02x bitpool %d (want 44.1 kHz stereo, 16 blocks, 8 subbands, loudness, 53)", H.conf[0], H.conf[1], H.conf[3]);
    CHECK(H.opened == 1 && H.started == 1, "opened %d, started %d", H.opened, H.started);
    CHECK(sound_out != NULL && strstr(bt_audio_status(), "Playing on"), "status: %s", bt_audio_status());
    int want_frames = 2000 * 44100 / 1000 / 128;
    CHECK(H.frames >= want_frames - 10 && H.frames <= want_frames + 30, "%d SBC frames in 2 s (want about %d)", H.frames, want_frames);
    CHECK(!H.seq_errs && !H.bad_rtp, "RTP: %d sequence gaps, %d bad headers", H.seq_errs, H.bad_rtp);
    printf("  headphones: paired, encrypted, SDP answered, SBC 44.1 kHz stereo bitpool 53, %d packets / %d frames in 2 s\n", H.packets, H.frames);
    const char *ff = getenv("FFMPEG");
    if (ff) {                                                  /* what the headphones would play */
        char cmd[512];
        snprintf(cmd, sizeof cmd, "%s -hide_banner -loglevel error -y -f sbc -i build/test_bt_a2dp.sbc -f s16le -ac 1 build/test_bt_a2dp.raw", ff);
        CHECK(system(cmd) == 0, "ffmpeg could not decode the A2DP stream");
        FILE *r = fopen("build/test_bt_a2dp.raw", "rb");
        static i16 pcm[44100 * 3];
        int n = r ? (int)fread(pcm, 2, 44100 * 3, r) : 0;
        if (r) fclose(r);
        int cross = 0, first = -1, last = -1;
        for (int i = 4410; i < n; i++) if (pcm[i - 1] < 0 && pcm[i] >= 0) { if (first < 0) first = i; last = i; cross++; }
        double hz = cross > 1 ? (double)(cross - 1) * 44100 / (last - first) : 0;
        CHECK(fabs(hz - 1000) < 2, "decoded A2DP audio: %.1f Hz (want 1000)", hz);
        printf("  decoded by ffmpeg: %d samples, %.1f Hz\n", n, hz);
    }
    bt_audio_disconnect();
    for (int t = 0; t < 100; t++) { sim_ms++; bt_poll_once(); }
    CHECK(H.disconnected == 1 && !sound_out && strstr(bt_audio_status(), "disconnected"), "disconnected: %s", bt_audio_status());
    /* again: the stored key, no pairing */
    memset(&H.ch, 0, sizeof H.ch);
    H.started = H.configured = 0; H.paired_ssp = 0;
    bt_audio_connect(HS);
    for (int t = 0; t < 3000 && !H.started; t++) { sim_ms++; bt_poll_once(); if (sound_out && sim_ms % 4 == 0) sound_out->pump(sound_out); }
    CHECK(H.key_replies == 1 && H.key_ok && !H.paired_ssp, "reconnect with the stored link key (replies %d, key ok %d)", H.key_replies, H.key_ok);
    CHECK(H.started == 1 && sound_out != NULL, "streaming again after reconnecting");

    if (fails) { printf("test_bt: %d failures\n", fails); return 1; }
    printf("test_bt: Intel firmware download, boot, scan, pairing, A2DP streaming and reconnecting pass\n");
    return 0;
}
