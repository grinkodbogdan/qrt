/* test_sound.c - the sound core's mixer on the host (make check): resampling keeps
 * the pitch, the volume scales, sums clip, and the played position counts. */
#define SND_HOST_TEST
#include "../src/kernel/sound.c"
#include <math.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* frequency from zero crossings, amplitude from the peak */
static void measure(const i16 *x, int n, int stride, int rate, double *hz, int *peak) {
    int cross = 0, first = -1, last = -1, pk = 0;
    for (int i = 1; i < n; i++) {
        if (x[(i - 1) * stride] < 0 && x[i * stride] >= 0) { if (first < 0) first = i; last = i; cross++; }
        int a = abs(x[i * stride]);
        if (a > pk) pk = a;
    }
    *hz = cross > 1 ? (double)(cross - 1) * rate / (last - first) : 0;
    *peak = pk;
}

int main(void) {
    static i16 in[44100], out[2 * 48000];
    /* a 1 kHz tone at 44.1 kHz, mono, half scale */
    for (int i = 0; i < 44100; i++) in[i] = (i16)(16384 * sin(2 * M_PI * 1000 * i / 44100.0));
    snd_stream_t *s = snd_open();
    CHECK(s, "open");
    snd_config(s, 44100, 1);
    int took = snd_write(s, in, 20000 * 2);
    CHECK(took == 40000, "write took %d", took);
    CHECK(snd_queued(s) == 40000, "queued %d", snd_queued(s));
    snd_set_volume(100);
    snd_mix(out, 21000, 48000, 2);                            /* 20000 / 44100 * 48000 = 21768 frames available */
    double hz; int pk;
    measure(out, 21000, 2, 48000, &hz, &pk);
    CHECK(fabs(hz - 1000) < 2, "resampled pitch %.2f Hz", hz);
    CHECK(pk > 16000 && pk <= 16400, "peak %d at full volume", pk);
    measure(out + 1, 21000, 2, 48000, &hz, &pk);
    CHECK(fabs(hz - 1000) < 2, "right channel %.2f Hz", hz);
    u64 played = snd_played(s);
    CHECK(played > 38500 * 1 && played <= 40000, "played %llu bytes", (unsigned long long)played);

    /* half volume: a quarter of the amplitude (square curve) */
    snd_reset(s);
    snd_write(s, in, 8000 * 2);
    snd_set_volume(50);
    snd_mix(out, 8000, 48000, 2);
    measure(out, 8000, 2, 48000, &hz, &pk);
    CHECK(pk > 3900 && pk < 4200, "peak %d at half volume", pk);

    /* two full-scale streams clip instead of wrapping */
    snd_reset(s);
    snd_config(s, 48000, 2);
    snd_stream_t *t = snd_open();
    static i16 loud[2 * 4800];
    for (int i = 0; i < 2 * 4800; i++) loud[i] = 30000;
    snd_write(s, loud, sizeof loud);
    snd_write(t, loud, sizeof loud);
    snd_set_volume(100);
    snd_mix(out, 4000, 48000, 2);
    CHECK(out[100] == 32767 && out[101] == 32767, "clipped to %d %d", out[100], out[101]);

    /* a closed stream plays out, then its slot is free again */
    snd_close(t);
    snd_mix(out, 2000, 48000, 2);
    CHECK(!t->used, "closed stream freed after playing");
    snd_close(s);
    snd_mix(out, 2000, 48000, 2);                             /* s plays out too */

    /* the chime: three notes, then the stream is gone */
    snd_beep();
    int busy = 0;
    for (int i = 0; i < SND_MAX; i++) busy += streams[i].used;
    CHECK(busy == 1, "beep stream open (%d)", busy);
    snd_mix(out, 48000, 48000, 2);
    measure(out, 6000, 2, 48000, &hz, &pk);
    CHECK(fabs(hz - 1047) < 15, "chime first note %.1f Hz", hz);
    busy = 0;
    for (int i = 0; i < SND_MAX; i++) busy += streams[i].used;
    CHECK(busy == 0, "beep stream freed (%d)", busy);

    if (!fails) printf("sound: resampling, volume, clipping, playback position, chime - ok\n");
    return fails != 0;
}
