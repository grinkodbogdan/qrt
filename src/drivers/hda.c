/*
 * hda.c - Intel High Definition Audio: the sound of most PCs and laptops (the Panasonic
 * FZ-G1's Realtek codec, QEMU's intel-hda).
 *
 * Written from Intel's "High Definition Audio Specification" (rev 1.0a), as Linux's
 * snd-hda-intel and OpenBSD's azalia(4) drive it, polled (no interrupts):
 *   - the controller: reset, the command ring (CORB) and the response ring (RIRB);
 *   - each codec's audio function group: every output pin that is connected (speaker,
 *     headphones, line out) and a path from it through mixers and selectors to a DAC;
 *     amplifiers on the way unmuted at 0 dB, the pins enabled (headphone amp, EAPD);
 *   - one output stream, 48 kHz 16-bit stereo, that every chosen DAC takes (they share
 *     its stream tag), from a 16 KB ring the sound thread refills from the mixer.
 * Headphones and speakers play together for now (no jack sensing yet).
 */
#include "hda.h"
#include "pci.h"
#include "../kernel/sound.h"

#define RING_BYTES 16384                     /* 85 ms at 48 kHz stereo 16-bit */
#define NBDL       4

static struct {
    volatile u8 *m;                          /* registers */
    u32 *corb; u64 *rirb;
    u16 corb_wp, rirb_rp;
    int iss, oss, sd;                        /* the output stream's descriptor index */
    u8 *ring; u64 *bdl;
    u64 wpos, wraps; u32 last_lpib;
    int dacs, pins, codecs;
    char name[64], status[128];
    snd_output_t out;
    i16 mix[1024];
    int ok;
} H;

static u8  r8(u32 o)  { return *(volatile u8 *)(H.m + o); }
static u16 r16(u32 o) { return *(volatile u16 *)(H.m + o); }
static u32 r32(u32 o) { return *(volatile u32 *)(H.m + o); }
static void w8(u32 o, u8 v)   { *(volatile u8 *)(H.m + o) = v; }
static void w16(u32 o, u16 v) { *(volatile u16 *)(H.m + o) = v; }
static void w32(u32 o, u32 v) { *(volatile u32 *)(H.m + o) = v; }

enum { GCAP = 0x00, GCTL = 0x08, STATESTS = 0x0e, INTCTL = 0x20,
       CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4a, CORBCTL = 0x4c, CORBSIZE = 0x4e,
       RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5a, RIRBCTL = 0x5c, RIRBSTS = 0x5d, RIRBSIZE = 0x5e };
#define SD(n, r) (0x80 + (n) * 0x20 + (r))
enum { SD_CTL = 0x00, SD_STS = 0x03, SD_LPIB = 0x04, SD_CBL = 0x08, SD_LVI = 0x0c, SD_FMT = 0x12, SD_BDPL = 0x18, SD_BDPU = 0x1c };

static int wait_bits(u32 off, int bytes, u32 mask, u32 want, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        u32 v = bytes == 1 ? r8(off) : bytes == 2 ? r16(off) : r32(off);
        if ((v & mask) == want) return 1;
        hal_delay_us(100);
    }
    return 0;
}

/* ---- codec commands --------------------------------------------------------------------- */
static int cmd(u32 verb, u32 *resp) {
    u16 rp_before = r16(RIRBWP) & 0xff;
    H.corb_wp = (u16)((H.corb_wp + 1) & 0xff);
    H.corb[H.corb_wp] = verb;
    w16(CORBWP, H.corb_wp);
    for (int i = 0; i < 2000; i++) {                         /* up to 200 ms */
        u16 wp = r16(RIRBWP) & 0xff;
        if (wp != rp_before) {
            H.rirb_rp = (u16)((H.rirb_rp + 1) & 0xff);
            u32 r = (u32)H.rirb[H.rirb_rp];
            w8(RIRBSTS, 0x05);
            if (resp) *resp = r;
            return 1;
        }
        hal_delay_us(100);
    }
    return 0;
}
static u32 verb12(int cad, int nid, u32 v, u32 payload) { return (u32)cad << 28 | (u32)nid << 20 | v << 8 | (payload & 0xff); }
static u32 verb4(int cad, int nid, u32 v, u32 payload) { return (u32)cad << 28 | (u32)nid << 20 | v << 16 | (payload & 0xffff); }
static u32 param(int cad, int nid, int p) { u32 r = 0; if (!cmd(verb12(cad, nid, 0xf00, (u32)p), &r)) return 0xffffffffu; return r; }
static void set12(int cad, int nid, u32 v, u32 payload) { cmd(verb12(cad, nid, v, payload), NULL); }
static void set4(int cad, int nid, u32 v, u32 payload) { cmd(verb4(cad, nid, v, payload), NULL); }

/* ---- widgets ---------------------------------------------------------------------------- */
enum { W_OUT = 0, W_IN = 1, W_MIX = 2, W_SEL = 3, W_PIN = 4 };
#define MAXN 128
static int afg;                              /* the audio function group being set up: default amp caps */
static int wtype(int cad, int nid) { return (int)((param(cad, nid, 0x09) >> 20) & 0xf); }

static int conns(int cad, int nid, int *out, int max) {
    u32 len = param(cad, nid, 0x0e);
    if (len == 0xffffffffu) return 0;
    int n = (int)(len & 0x7f), lng = (len >> 7) & 1, per = lng ? 2 : 4, got = 0;
    for (int i = 0; i < n && got < max; i += per) {
        u32 r = 0;
        if (!cmd(verb12(cad, nid, 0xf02, (u32)i), &r)) break;
        for (int j = 0; j < per && i + j < n && got < max; j++) {
            u32 e = lng ? (r >> (16 * j)) & 0xffff : (r >> (8 * j)) & 0xff;
            int range = lng ? (e >> 15) & 1 : (e >> 7) & 1;
            int id = (int)(lng ? e & 0x7fff : e & 0x7f);
            if (range && got > 0) {                          /* a range from the previous entry */
                for (int x = out[got - 1] + 1; x <= id && got < max; x++) out[got++] = x;
            } else out[got++] = id;
        }
    }
    return got;
}

/* unmute a widget's amps at 0 dB: the output amp, and input amp 'idx' (-1: all) */
static void unmute(int cad, int nid, int idx) {
    u32 wc = param(cad, nid, 0x09);
    if ((wc >> 2) & 1) {                                     /* output amp present */
        u32 caps = (wc >> 3) & 1 ? param(cad, nid, 0x12) : param(cad, afg, 0x12);
        set4(cad, nid, 0x3, 0xb000 | (caps & 0x7f));         /* output, left+right, unmuted, 0 dB */
    }
    if ((wc >> 1) & 1) {                                     /* input amp present */
        u32 caps = (wc >> 3) & 1 ? param(cad, nid, 0x0d) : param(cad, afg, 0x0d);
        if (idx < 0) for (int i = 0; i < 16; i++) set4(cad, nid, 0x3, 0x7000 | (u32)i << 8 | (caps & 0x7f));
        else set4(cad, nid, 0x3, 0x7000 | (u32)idx << 8 | (caps & 0x7f));
    }
}

/* depth-first: from nid to a DAC; sets each node's connection on the way back. returns the DAC or 0 */
static int route(int cad, int nid, int depth) {
    if (depth > 6) return 0;
    int c[32];
    int n = conns(cad, nid, c, 32);
    for (int i = 0; i < n; i++) {
        int t = wtype(cad, c[i]);
        int dac = 0;
        if (t == W_OUT) dac = c[i];
        else if (t == W_MIX || t == W_SEL) dac = route(cad, c[i], depth + 1);
        if (!dac) continue;
        int nt = wtype(cad, nid);
        if (nt != W_MIX && n > 1) set12(cad, nid, 0x701, (u32)i);   /* selectors and pins pick the input */
        set12(cad, nid, 0x705, 0);                                     /* power D0 */
        unmute(cad, nid, nt == W_MIX ? i : -1);
        if (t == W_OUT) { set12(cad, c[i], 0x705, 0); unmute(cad, c[i], -1); }
        return dac;
    }
    return 0;
}

static void setup_codec(int cad) {
    u32 vid = param(cad, 0, 0x00);
    u32 sub = param(cad, 0, 0x04);
    int start = (int)((sub >> 16) & 0xff), cnt = (int)(sub & 0xff);
    klog("hda: codec %d: %04x:%04x", cad, vid >> 16, vid & 0xffff);
    if (!H.name[0]) fmt(H.name, sizeof H.name, "HD Audio (codec %04x:%04x)", vid >> 16, vid & 0xffff);
    for (int fg = start; fg < start + cnt; fg++) {
        if ((param(cad, fg, 0x05) & 0xff) != 1) continue;    /* the audio function group */
        afg = fg;
        set12(cad, fg, 0x705, 0);                            /* D0 */
        hal_delay_us(10000);
        u32 ws = param(cad, fg, 0x04);
        int w0 = (int)((ws >> 16) & 0xff), wn = (int)(ws & 0xff);
        for (int nid = w0; nid < w0 + wn && nid < w0 + MAXN; nid++) {
            if (wtype(cad, nid) != W_PIN) continue;
            u32 pc = param(cad, nid, 0x0c);
            if (!((pc >> 4) & 1)) continue;                  /* not output capable */
            u32 cfg = 0;
            cmd(verb12(cad, nid, 0xf1c, 0), &cfg);
            int conn = (int)(cfg >> 30), dev = (int)((cfg >> 20) & 0xf);
            if (conn == 1) continue;                         /* nothing attached */
            if (dev != 0 && dev != 1 && dev != 2) continue;  /* line out, speaker, headphones */
            int dac = route(cad, nid, 0);
            if (!dac) continue;
            set12(cad, dac, 0x706, 1 << 4);                  /* stream tag 1, channel 0 */
            set4(cad, dac, 0x2, 0x0011);                     /* 48 kHz, 16-bit, 2 channels */
            set12(cad, nid, 0x707, 0x40 | (dev == 2 && ((pc >> 3) & 1) ? 0x80 : 0));   /* out (+ headphone amp) */
            if ((pc >> 16) & 1) set12(cad, nid, 0x70c, 0x02);   /* EAPD: an external amplifier on */
            klog("hda: codec %d: %s at node %d from DAC %d", cad, dev == 1 ? "speaker" : dev == 2 ? "headphones" : "line out", nid, dac);
            H.dacs++; H.pins++;
        }
    }
}

/* ---- the stream ------------------------------------------------------------------------- */
static void pump(snd_output_t *o) {
    (void)o;
    u32 lpib = r32(SD(H.sd, SD_LPIB)) % RING_BYTES;
    if (lpib < H.last_lpib) H.wraps++;
    H.last_lpib = lpib;
    u64 read = H.wraps * RING_BYTES + lpib;
    u64 target = read + RING_BYTES / 2;
    if (H.wpos < read) H.wpos = read;                        /* fell behind: skip ahead */
    while (H.wpos < target) {
        u32 at = (u32)(H.wpos % RING_BYTES);
        u32 n = (u32)MIN(target - H.wpos, (u64)(RING_BYTES - at));
        n = MIN(n, (u32)sizeof H.mix) & ~3u;
        if (!n) break;
        snd_mix(H.mix, (int)(n / 4), 48000, 2);
        memcpy(H.ring + at, H.mix, n);
        H.wpos += n;
    }
}

static int start_stream(void) {
    H.sd = H.iss;                                            /* the first output stream descriptor */
    u32 ctl = SD(H.sd, SD_CTL);
    w8(ctl, r8(ctl) | 1);                                    /* stream reset */
    if (!wait_bits(ctl, 1, 1, 1, 10)) return 0;
    w8(ctl, r8(ctl) & ~1);
    if (!wait_bits(ctl, 1, 1, 0, 10)) return 0;
    H.ring = hal_dma_alloc(RING_BYTES);
    H.bdl = hal_dma_alloc(4096);
    for (int i = 0; i < NBDL; i++) {
        u64 a = (u64)(usize)H.ring + (u64)i * (RING_BYTES / NBDL);
        H.bdl[i * 2] = a;
        H.bdl[i * 2 + 1] = (u64)(RING_BYTES / NBDL);         /* length; no interrupt on completion */
    }
    w32(SD(H.sd, SD_BDPL), (u32)(usize)H.bdl);
    w32(SD(H.sd, SD_BDPU), (u32)((u64)(usize)H.bdl >> 32));
    w32(SD(H.sd, SD_CBL), RING_BYTES);
    w16(SD(H.sd, SD_LVI), NBDL - 1);
    w16(SD(H.sd, SD_FMT), 0x0011);                           /* 48 kHz, 16-bit, stereo */
    w8(ctl + 2, (u8)((r8(ctl + 2) & 0x0f) | (1 << 4)));      /* stream tag 1 (bits 20-23) */
    pump(&H.out);                                            /* half a ring of silence (or sound) first */
    w8(ctl, r8(ctl) | 0x02);                                 /* run */
    return 1;
}

void hda_probe(const pci_dev_t *p) {
    if (H.ok) { strlcpy(H.status, "another HD Audio controller (one is used)", sizeof H.status); return; }
    u32 cmdreg = pci_read32(p->bus, p->dev, p->fn, 4);
    pci_write32(p->bus, p->dev, p->fn, 4, cmdreg | 0x6);     /* memory, bus master */
    H.m = (volatile u8 *)(usize)pci_bar(p->bus, p->dev, p->fn, 0);
    if (!H.m) { strlcpy(H.status, "no registers", sizeof H.status); return; }
    u16 gcap = r16(GCAP);
    H.iss = (gcap >> 8) & 0xf; H.oss = (gcap >> 12) & 0xf;
    if (!H.oss) { strlcpy(H.status, "no output streams", sizeof H.status); return; }

    /* controller reset */
    w32(GCTL, r32(GCTL) & ~1u);
    if (!wait_bits(GCTL, 4, 1, 0, 100)) { strlcpy(H.status, "did not enter reset", sizeof H.status); return; }
    hal_delay_us(200);
    w32(GCTL, r32(GCTL) | 1u);
    if (!wait_bits(GCTL, 4, 1, 1, 100)) { strlcpy(H.status, "did not leave reset", sizeof H.status); return; }
    hal_delay_us(2000);                                      /* codecs announce themselves (521 us min) */
    u16 codecs = r16(STATESTS);
    w16(STATESTS, codecs);
    w32(INTCTL, 0);

    /* CORB and RIRB, 256 entries each */
    w8(CORBCTL, 0); w8(RIRBCTL, 0);
    wait_bits(CORBCTL, 1, 2, 0, 10); wait_bits(RIRBCTL, 1, 2, 0, 10);
    H.corb = hal_dma_alloc(4096);
    H.rirb = hal_dma_alloc(4096);
    w32(CORBLBASE, (u32)(usize)H.corb); w32(CORBUBASE, (u32)((u64)(usize)H.corb >> 32));
    w32(RIRBLBASE, (u32)(usize)H.rirb); w32(RIRBUBASE, (u32)((u64)(usize)H.rirb >> 32));
    w8(CORBSIZE, 2); w8(RIRBSIZE, 2);
    w16(CORBRP, 0x8000);                                     /* read pointer reset */
    wait_bits(CORBRP, 2, 0x8000, 0x8000, 10);
    w16(CORBRP, 0);
    wait_bits(CORBRP, 2, 0x8000, 0, 10);
    w16(CORBWP, 0); H.corb_wp = 0;
    w16(RIRBWP, 0x8000); H.rirb_rp = 0;
    w16(RINTCNT, 1);
    w8(CORBCTL, 2); w8(RIRBCTL, 3);                          /* run both; RIRB status bit on (no interrupt: INTCTL is 0) */

    for (int cad = 0; cad < 15; cad++)
        if ((codecs >> cad) & 1) { H.codecs++; setup_codec(cad); }
    if (!H.dacs) { fmt(H.status, sizeof H.status, "%d codec(s), no output found", H.codecs); return; }
    if (!start_stream()) { strlcpy(H.status, "the output stream did not reset", sizeof H.status); return; }
    H.ok = 1;
    H.out = (snd_output_t){ H.name, 48000, 2, pump, NULL };
    snd_output_add(&H.out);
    fmt(H.status, sizeof H.status, "playing: %d output(s), 48000 Hz", H.pins);
    klog("hda: %s: %s", H.name, H.status);
}

const char *hda_status(void) { return H.status[0] ? H.status : "not probed"; }
