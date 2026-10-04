/*
 * sound.c - the sound core: streams, the mixer, the volume, the sound thread.
 *
 * A stream keeps its frames as 16-bit stereo in a ring, whatever was written (mono is
 * doubled).  The mixer reads every stream at the output's rate: a 32.32 fixed-point
 * position steps through the ring by in_rate / out_rate and interpolates linearly
 * between neighbouring frames - plain, but enough for 44.1 <-> 48 kHz.  The sum is
 * scaled by the volume (a square curve, so the low half of the slider is usable) and
 * clipped.
 *
 * Everything that touches a ring runs on the boot core (the sound thread, the
 * output drivers' pumps, programs' writes), so turning interrupts off is the lock.
 * Built on the host as well, for make check (SND_HOST_TEST).
 */
#include "sound.h"

#ifdef SND_HOST_TEST
#include <stdlib.h>
#define kalloc(n) calloc(1, (n))
#define kfree free
#define LOCK()    (void)0
#define UNLOCK()  (void)0
static void klog(const char *f, ...) { (void)f; }
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define strlcpy(d, s, n) snprintf((d), (n), "%s", (s))
#include <stdio.h>
#define fmt snprintf
#else
#if defined(__x86_64__)
#include "../arch/x64/cpu.h"
#include "../arch/x64/sched.h"
#define LOCK()    u64 snd_fl_ = irq_save()
#define UNLOCK()  irq_restore(snd_fl_)
#else
#define LOCK()    (void)0
#define UNLOCK()  (void)0
#endif
#endif

struct snd_stream {
    int used, closing;
    int rate, channels;
    i16 *ring;                     /* SND_RING stereo frames */
    volatile u32 head, tail;       /* frames written / consumed (free-running) */
    u32 frac;                      /* position between frame tail and tail + 1 (of 2^32) */
    u64 played;                    /* frames consumed since open or reset */
};

static snd_stream_t streams[SND_MAX];
static snd_output_t *outputs;      /* the first one plays */
static int volume = 50;
static u64 last_tick;
static char status[96];

/* ---- streams ------------------------------------------------------------------------- */
snd_stream_t *snd_open(void) {
    LOCK();
    snd_stream_t *s = NULL;
    for (int i = 0; i < SND_MAX; i++) if (!streams[i].used) { s = &streams[i]; break; }
    if (s) { s->used = 1; s->closing = 0; }
    UNLOCK();
    if (!s) return NULL;
    if (!s->ring) s->ring = kalloc(SND_RING * 4);
    s->rate = SND_RATE; s->channels = 2;
    s->head = s->tail = 0; s->frac = 0; s->played = 0;
    return s;
}

void snd_close(snd_stream_t *s) {
    if (!s) return;
    LOCK();
    if (s->head == s->tail) s->used = 0;                     /* nothing left to play */
    else s->closing = 1;                                      /* the mixer frees it once played */
    UNLOCK();
}

int snd_config(snd_stream_t *s, int rate, int channels) {
    if (rate < 4000 || rate > 192000 || (channels != 1 && channels != 2)) return -1;
    LOCK();
    s->rate = rate; s->channels = channels;
    UNLOCK();
    return 0;
}

int snd_rate(snd_stream_t *s) { return s->rate; }
int snd_channels(snd_stream_t *s) { return s->channels; }

int snd_write(snd_stream_t *s, const void *pcm, int bytes) {
    const i16 *in = pcm;
    int fsz = 2 * s->channels;
    int want = bytes / fsz;
    u32 room;
    { LOCK(); room = SND_RING - (s->head - s->tail); UNLOCK(); }
    int n = MIN(want, (int)room);
    u32 h = s->head;
    for (int i = 0; i < n; i++, h++) {
        i16 *d = &s->ring[(h % SND_RING) * 2];
        if (s->channels == 2) { d[0] = in[2 * i]; d[1] = in[2 * i + 1]; }
        else d[0] = d[1] = in[i];
    }
    { LOCK(); s->head = h; UNLOCK(); }                        /* published after the samples */
    return n * fsz;
}

int snd_space(snd_stream_t *s) { return (int)(SND_RING - (s->head - s->tail)) * 2 * s->channels; }
int snd_queued(snd_stream_t *s) { return (int)(s->head - s->tail) * 2 * s->channels; }
u64 snd_played(snd_stream_t *s) { return s->played * 2 * (u64)s->channels; }

void snd_reset(snd_stream_t *s) {
    LOCK();
    s->tail = s->head; s->frac = 0; s->played = 0;
    UNLOCK();
}

/* ---- the mixer --------------------------------------------------------------------------- */
/* add up to frames frames of s, at out_rate, into acc (stereo); returns how many it had */
static int mix_stream(snd_stream_t *s, i32 *acc, int frames, int out_rate) {
    u64 step = ((u64)s->rate << 32) / (u64)out_rate;
    u32 tail = s->tail, head = s->head;
    u64 pos = s->frac;
    int i = 0;
    for (; i < frames; i++) {
        if (head - tail < 2) break;                           /* interpolation needs the next frame */
        const i16 *a = &s->ring[(tail % SND_RING) * 2], *b = &s->ring[((tail + 1) % SND_RING) * 2];
        i32 f = (i32)(pos >> 16);                             /* 0..65535 */
        acc[2 * i]     += a[0] + (((b[0] - a[0]) * f) >> 16);
        acc[2 * i + 1] += a[1] + (((b[1] - a[1]) * f) >> 16);
        pos += step;
        u32 adv = (u32)(pos >> 32);
        pos &= 0xffffffffu;
        tail += adv;
        s->played += adv;
    }
    /* the last frame of a stream that is closing still sounds */
    if (i < frames && s->closing && head - tail == 1) {
        const i16 *a = &s->ring[(tail % SND_RING) * 2];
        acc[2 * i] += a[0]; acc[2 * i + 1] += a[1];
        tail++; s->played++; i++;
    }
    s->tail = tail;
    s->frac = (u32)pos;
    return i;
}

void snd_mix(i16 *out, int frames, int rate, int channels) {
    static i32 acc[2 * 1024];
    i32 gain = volume * volume * 65536 / 10000;               /* Q16, square curve */
    while (frames > 0) {
        int n = MIN(frames, 1024);
        memset(acc, 0, sizeof(i32) * 2 * (usize)n);
        LOCK();
        for (int k = 0; k < SND_MAX; k++) {
            snd_stream_t *s = &streams[k];
            if (!s->used) continue;
            mix_stream(s, acc, n, rate);
            if (s->closing && s->head == s->tail) s->used = 0;
        }
        UNLOCK();
        for (int i = 0; i < n; i++) {
            i32 l = (i32)(((i64)acc[2 * i] * gain) >> 16), r = (i32)(((i64)acc[2 * i + 1] * gain) >> 16);
            l = CLAMP(l, -32768, 32767); r = CLAMP(r, -32768, 32767);
            if (channels == 2) { out[2 * i] = (i16)l; out[2 * i + 1] = (i16)r; }
            else out[i] = (i16)((l + r) / 2);
        }
        out += n * channels;
        frames -= n;
    }
}

/* ---- outputs, volume ----------------------------------------------------------------------- */
void snd_output_add(snd_output_t *o) {
    LOCK();
    o->next = outputs;
    outputs = o;
    UNLOCK();
    klog("sound: output %s, %d Hz, %d channel%s", o->name, o->rate, o->channels, o->channels == 1 ? "" : "s");
}

void snd_output_remove(snd_output_t *o) {
    LOCK();
    for (snd_output_t **p = &outputs; *p; p = &(*p)->next) if (*p == o) { *p = o->next; break; }
    UNLOCK();
    klog("sound: output %s gone", o->name);
}

/* which output plays: the one chosen in Settings while it is there; otherwise Bluetooth, then USB,
 * then the built-in speaker (newest first within each) */
static char chosen[48];               /* "" = automatic; else the output's name */
static int out_rank(const snd_output_t *o) { return !strncmp(o->name, "Bluetooth", 9) ? 3 : !strncmp(o->name, "USB", 3) ? 2 : 1; }
static snd_output_t *current(void) {
    snd_output_t *best = NULL;
    for (snd_output_t *o = outputs; o; o = o->next) {
        if (chosen[0] && !strcmp(o->name, chosen)) return o;
        if (!best || out_rank(o) > out_rank(best)) best = o;
    }
    return best;
}
int snd_outputs(const char **names, int max) {
    int n = 0;
    LOCK();
    for (snd_output_t *o = outputs; o && n < max; o = o->next) names[n++] = o->name;
    UNLOCK();
    return n;
}
void snd_choose_output(const char *name) { LOCK(); strlcpy(chosen, name ? name : "", sizeof chosen); UNLOCK(); klog("sound: output %s", name && name[0] ? name : "automatic"); }
const char *snd_chosen_output(void) { return chosen; }
const char *snd_output_name(void) { snd_output_t *o = current(); return o ? o->name : ""; }
int snd_volume(void) { return volume; }
void snd_set_volume(int v) { volume = CLAMP(v, 0, 100); }

const char *snd_status(void) {
    int n = 0;
    for (int i = 0; i < SND_MAX; i++) n += streams[i].used;
    snd_output_t *o = current();
    if (o) fmt(status, sizeof status, "%s, %d Hz; %d stream%s playing", o->name, o->rate, n, n == 1 ? "" : "s");
    else fmt(status, sizeof status, "no output yet: USB audio or Bluetooth headphones (%d stream%s)", n, n == 1 ? "" : "s");
    return status;
}

/* no output: the streams are played into nothing, in real time */
void snd_tick(u64 now_us) {
    static i16 scratch[2 * 1024];
    if (!last_tick || now_us - last_tick > 250000) last_tick = now_us;
    u64 frames = (now_us - last_tick) * SND_RATE / 1000000;
    if (!frames) return;
    last_tick += frames * 1000000 / SND_RATE;
    while (frames) {
        int n = (int)MIN(frames, 1024);
        snd_mix(scratch, n, SND_RATE, 2);
        frames -= (u64)n;
    }
}

/* a short chime: three rising notes */
void snd_beep(void) {
    snd_stream_t *s = snd_open();
    if (!s) return;
    static const int hz[3] = { 1047, 1319, 1568 };            /* C6, E6, G6 */
    static i16 buf[SND_RATE / 8];
    for (int note = 0; note < 3; note++) {
        int n = SND_RATE / 8;                                 /* 125 ms each, mono */
        u32 phase = 0, inc = (u32)((u64)hz[note] * 65536 / SND_RATE);
        for (int i = 0; i < n; i++) {
            /* a triangle wave (no sin() in the kernel), with a fall-off envelope */
            u32 p = (phase >> 0) & 0xffff;
            i32 tri = p < 32768 ? (i32)p * 2 - 32768 : 98303 - (i32)p * 2;
            i32 env = 32767 - i * 32767 / n;
            buf[i] = (i16)(tri * env / 32767 / 3);
            phase += inc;
        }
        snd_config(s, SND_RATE, 1);
        snd_write(s, buf, n * 2);
    }
    snd_close(s);
}

#if !defined(SND_HOST_TEST) && defined(__x86_64__)
static void sound_thread(void *arg) {
    (void)arg;
    for (;;) {
        u64 fl = irq_save();
        snd_output_t *o = current();
        irq_restore(fl);
        if (o && o->pump) o->pump(o);
        else snd_tick(k_now_us());
        thread_sleep_ms(4);
    }
}

void snd_init(void) {
    if (!k.native) return;
    thread_set_prio(thread_create("sound", sound_thread, NULL, 0));
    klog("sound: mixer ready, %s", snd_status());
}
#else
void snd_init(void) {}
#endif
