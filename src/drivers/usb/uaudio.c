/*
 * uaudio.c - USB audio playback: the USB Audio Class, versions 1 and 2.
 *
 * A USB headset, a USB-C to 3.5 mm dongle or a dock's headphone jack is an Audio
 * Control interface (class 1, subclass 1) and Audio Streaming interfaces (subclass 2).
 * A streaming interface's alternate setting 0 has no bandwidth; the others each carry
 * one format (channels, sample size, rates) and an isochronous endpoint.  We take the
 * one that plays: an OUT endpoint, PCM, 1 or 2 channels, 16/24/32-bit samples, at
 * 48 kHz if it can (else 44.1 kHz, else its first rate), switch to it with
 * SET_INTERFACE, set the rate (UAC1: on the endpoint; UAC2: on the clock source), turn
 * the feature unit's mute off and its volume to 0 dB - the sound core does the volume -
 * and become the sound output.  The sound thread then keeps 40 ms of packets queued,
 * each the mix for one service interval (1 ms at full speed).
 *
 * Written from the USB Audio Class 1.0 and 2.0 specifications; Linux's sound/usb was
 * the reference for what devices do in practice (e.g. stalls on optional requests).
 * Asynchronous endpoints' feedback is not read: the nominal rate is sent, so a device
 * whose clock drifts a little will drop or repeat a sample now and then.
 */
#include "uaudio.h"
#include "../../kernel/sound.h"

#if defined(__x86_64__)
#define MAX_UA 2

typedef struct {
    int used;
    udev_t *d;
    int v2;                                /* Audio Class 2.0 */
    int ac_iface, as_iface, alt;
    u8 ep;
    int mps, binterval;
    int channels, subframe, bits, rate;
    int fu, clock;                         /* feature unit and (UAC2) clock source ids, 0 = none */
    u32 frac;                              /* frames per packet: the fraction carried over */
    u32 us_per_packet;
    char name[64];
    snd_output_t out;
    i16 mix[2 * 1024];
} ua_t;

static ua_t ua[MAX_UA];

/* ---- the stream ---------------------------------------------------------------------------- */
static int fill(void *arg, u8 *pkt, int max) {
    ua_t *a = arg;
    /* frames for this packet: rate x interval, the remainder carried to the next one */
    u64 num = (u64)a->rate * a->us_per_packet + a->frac;
    int frames = (int)(num / 1000000);
    a->frac = (u32)(num % 1000000);
    int fsz = a->channels * a->subframe;
    frames = MIN(frames, max / fsz);
    frames = MIN(frames, 1024);
    snd_mix(a->mix, frames, a->rate, a->channels);
    int n = frames * a->channels;
    if (a->subframe == 2) memcpy(pkt, a->mix, (usize)n * 2);
    else for (int i = 0; i < n; i++) {                                 /* 24/32-bit slots: the sample in the top bits */
        i32 v = (i32)a->mix[i] << (a->subframe * 8 - 16);
        for (int b = 0; b < a->subframe; b++) pkt[i * a->subframe + b] = (u8)(v >> (8 * b));
    }
    return frames * fsz;
}

static void pump(snd_output_t *o) {
    ua_t *a = (ua_t *)((u8 *)o - __builtin_offsetof(ua_t, out));
    if (!a->used) return;
    int ahead = (int)(40000 / a->us_per_packet);                      /* 40 ms queued */
    usb_iso_pump(a->d, a->ep, MAX(ahead, 8));
}

/* ---- class requests ----------------------------------------------------------------------------- */
static void set_rate(ua_t *a) {
    u8 b[4] = { (u8)a->rate, (u8)(a->rate >> 8), (u8)(a->rate >> 16), (u8)(a->rate >> 24) };
    if (a->v2) {
        if (a->clock) usb_control(a->d, 0x21, 0x01, 0x0100, (u16)(a->clock << 8 | a->ac_iface), b, 4);   /* CUR, SAM_FREQ */
    } else usb_control(a->d, 0x22, 0x01, 0x0100, a->ep, b, 3);           /* SET_CUR, SAMPLING_FREQ (may stall: fixed rate) */
}

static void unmute(ua_t *a) {
    if (!a->fu) return;
    u8 m = 0, v[2] = { 0, 0 };                                          /* mute off; volume 0 dB */
    u16 idx = (u16)(a->fu << 8 | a->ac_iface);
    for (int ch = 0; ch <= a->channels; ch++) {                         /* master, then each channel */
        usb_control(a->d, 0x21, 0x01, (u16)(0x0100 | ch), idx, &m, 1);
        usb_control(a->d, 0x21, 0x01, (u16)(0x0200 | ch), idx, v, 2);
    }
}

/* ---- descriptors ------------------------------------------------------------------------------ */
typedef struct { int iface, alt, v2, channels, subframe, bits, nrates, rates[8], rmin, rmax; u8 ep; int mps, ival, pcm; } alt_t;

static int score(const alt_t *s, int *rate) {
    if (!s->ep || !s->pcm || s->channels < 1 || s->channels > 2 || s->subframe < 2 || s->subframe > 4) return -1;
    int want[2] = { 48000, 44100 };
    *rate = 0;
    for (int w = 0; w < 2 && !*rate; w++) {
        for (int i = 0; i < s->nrates; i++) if (s->rates[i] == want[w]) *rate = want[w];
        if (!s->nrates && s->rmin <= want[w] && want[w] <= s->rmax) *rate = want[w];
    }
    if (!*rate) *rate = s->nrates ? s->rates[0] : s->rmin;
    if (*rate < 8000 || *rate > 192000) return -1;
    return (s->channels == 2) * 100 + (s->subframe == 2) * 10 + (*rate == 48000) * 5 + (*rate == 44100) * 3;
}

int uaudio_probe(udev_t *d) {
    int len;
    const u8 *c = usb_config(d, &len);
    if (!c) return 0;
    int ac = -1, v2 = 0, fu = 0, clock = 0, cur_if = -1, cur_alt = 0, cur_cls = 0, cur_sub = 0, cur_proto = 0;
    alt_t alts[8], *cur = NULL;
    int nalts = 0;
    for (int o = 0; o + 2 <= len && c[o] >= 2 && o + c[o] <= len; o += c[o]) {
        const u8 *p = c + o;
        int t = p[1], sub = p[0] >= 3 ? p[2] : 0;
        if (t == 4 && p[0] >= 9) {                                     /* interface */
            cur_if = p[2]; cur_alt = p[3]; cur_cls = p[5]; cur_sub = p[6]; cur_proto = p[7];
            cur = NULL;
            if (cur_cls == 1 && cur_sub == 1 && ac < 0) { ac = cur_if; v2 = cur_proto == 0x20; }
            if (cur_cls == 1 && cur_sub == 2 && cur_alt > 0 && nalts < 8) {
                cur = &alts[nalts++];
                memset(cur, 0, sizeof *cur);
                cur->iface = cur_if; cur->alt = cur_alt; cur->v2 = cur_proto == 0x20;
            }
        } else if (t == 0x24 && cur_cls == 1 && cur_sub == 1) {          /* Audio Control: units */
            if (sub == 6 && !fu) fu = p[3];                             /* the first feature unit */
            if (sub == 0x0a && v2 && !clock) clock = p[3];              /* UAC2 clock source */
        } else if (t == 0x24 && cur) {                                  /* Audio Streaming class descriptors */
            if (sub == 1) {                                             /* AS_GENERAL */
                if (cur->v2 && p[0] >= 16) { cur->pcm = p[5] == 1 && (p[6] & 1); cur->channels = p[10]; }
                else if (!cur->v2 && p[0] >= 7) cur->pcm = (p[5] | p[6] << 8) == 1;
            } else if (sub == 2 && p[3] == 1) {                         /* FORMAT_TYPE I */
                if (cur->v2 && p[0] >= 6) { cur->subframe = p[4]; cur->bits = p[5]; }
                else if (!cur->v2 && p[0] >= 8) {
                    cur->channels = p[4]; cur->subframe = p[5]; cur->bits = p[6];
                    int nf = p[7];
                    if (nf == 0 && p[0] >= 14) { cur->rmin = p[8] | p[9] << 8 | p[10] << 16; cur->rmax = p[11] | p[12] << 8 | p[13] << 16; }
                    for (int i = 0; i < nf && i < 8 && 8 + 3 * i + 2 < p[0]; i++)
                        cur->rates[cur->nrates++] = p[8 + 3 * i] | p[9 + 3 * i] << 8 | p[10 + 3 * i] << 16;
                }
            }
        } else if (t == 5 && cur && p[0] >= 7 && (p[3] & 3) == 1 && !(p[2] & 0x80) && !cur->ep) {   /* isochronous OUT */
            cur->ep = p[2]; cur->mps = p[4] | p[5] << 8; cur->ival = p[6];
        }
    }
    if (ac < 0) return 0;
    /* UAC2 rates come from the clock source; ask for 48 kHz, then 44.1 */
    for (int i = 0; i < nalts; i++) if (alts[i].v2) { alts[i].nrates = 2; alts[i].rates[0] = 48000; alts[i].rates[1] = 44100; }
    char prod[48];
    if (usb_product_string(d, prod, sizeof prod) <= 0) strlcpy(prod, "USB audio device", sizeof prod);
    ua_t *a = NULL;
    for (int i = 0; i < MAX_UA; i++) if (!ua[i].used) { a = &ua[i]; break; }
    if (!a) return 1;
    /* the settings by preference; one that the controller cannot schedule makes way for the next.
     * As Linux does: the controller's endpoint first (bandwidth), then SET_INTERFACE */
    int tried[8] = { 0 }, started = 0, ntry = 0;
    for (;;) {
        alt_t *best = NULL;
        int best_score = -1, best_rate = 0, bi = -1;
        for (int i = 0; i < nalts; i++) { int r, sc = score(&alts[i], &r); if (!tried[i] && sc > best_score) { best_score = sc; best = &alts[i]; best_rate = r; bi = i; } }
        if (!best) break;
        tried[bi] = 1; ntry++;
        memset(a, 0, sizeof *a);
        a->d = d; a->v2 = best->v2; a->ac_iface = ac; a->as_iface = best->iface; a->alt = best->alt;
        a->ep = best->ep; a->mps = best->mps; a->binterval = best->ival ? best->ival : 1;
        a->channels = best->channels; a->subframe = best->subframe; a->bits = best->bits; a->rate = best_rate;
        a->fu = fu; a->clock = clock;
        int hs = usb_speed(d) == 3 || usb_speed(d) == 4;
        a->us_per_packet = (u32)((hs ? 125 : 1000) << (CLAMP(a->binterval, 1, 16) - 1));
        if (usb_iso_open(d, a->ep, a->mps, a->binterval, fill, a)) { klog("usb: %s: setting %d/%d (%d Hz, %d ch) did not fit, trying another", prod, a->as_iface, a->alt, a->rate, a->channels); continue; }
        if (usb_set_interface(d, a->as_iface, a->alt)) { klog("usb: %s: SET_INTERFACE %d/%d failed", prod, a->as_iface, a->alt); continue; }
        set_rate(a);
        unmute(a);
        started = 1;
        break;
    }
    if (!started) {
        klog("usb: %s: no streaming setting started (%d tried of %d)", prod, ntry, nalts);
        usb_set_name(d, ntry ? "USB audio (stream did not start - see the log)" : "USB audio (no playback format QRT supports)");
        return 1;
    }
    a->used = 1;
    fmt(a->name, sizeof a->name, "USB audio: %s", prod);
    a->out = (snd_output_t){ a->name, a->rate, a->channels, pump, NULL };
    fmt(prod, sizeof prod, "%s (%s)", a->name + 11, a->v2 ? "UAC2" : "UAC1");
    usb_set_name(d, prod);
    klog("usb: %s: playing %d Hz, %d channel%s, %d-bit in %d-byte slots, a packet every %u us (UAC%d)", a->name + 11, a->rate,
         a->channels, a->channels == 1 ? "" : "s", a->bits, a->subframe, a->us_per_packet, a->v2 ? 2 : 1);
    snd_output_add(&a->out);
    return 1;
}

void uaudio_detach(udev_t *d) {
    for (int i = 0; i < MAX_UA; i++)
        if (ua[i].used && ua[i].d == d) { ua[i].used = 0; snd_output_remove(&ua[i].out); }
}

const char *uaudio_status(void) {
    for (int i = 0; i < MAX_UA; i++) if (ua[i].used) return ua[i].name;
    return NULL;
}
#else
int uaudio_probe(udev_t *d) { (void)d; return 0; }
void uaudio_detach(udev_t *d) { (void)d; }
const char *uaudio_status(void) { return NULL; }
#endif
