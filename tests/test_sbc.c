/* test_sbc.c - the SBC encoder (src/drivers/bt/sbc.c) against an independent decoder.
 * Encodes tones (1 kHz left, 440 Hz right, 44.1 kHz stereo, bitpool 53 - what A2DP
 * sinks get), decodes the stream with ffmpeg ($FFMPEG, if there is one) and compares:
 * every frame decodes, each channel has its pitch, and the signal-to-noise ratio after
 * lining up the codec's delay is high.  Without ffmpeg, only the frame layout is checked. */
#define SBC_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/drivers/bt/sbc.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define RATE 44100
#define SECS 2
static i16 in[RATE * SECS * 2];

static double pitch(const i16 *x, int n, int stride) {
    int first = -1, last = -1, cross = 0;
    for (int i = 1; i < n; i++) if (x[(i - 1) * stride] < 0 && x[i * stride] >= 0) { if (first < 0) first = i; last = i; cross++; }
    return cross > 1 ? (double)(cross - 1) * RATE / (last - first) : 0;
}

int main(void) {
    for (int i = 0; i < RATE * SECS; i++) {
        in[2 * i] = (i16)(12000 * sin(2 * M_PI * 1000 * i / RATE));
        in[2 * i + 1] = (i16)(9000 * sin(2 * M_PI * 440 * i / RATE));
    }
    sbc_t s;
    sbc_init(&s, RATE, 2, 16, 53);
    int flen = sbc_frame_len(&s), fs = sbc_samples(&s);
    CHECK(flen == 118 && fs == 128, "frame %d bytes, %d samples (want 118, 128)", flen, fs);
    const char *path = "build/test_sbc.sbc";
    FILE *f = fopen(path, "wb");
    int frames = RATE * SECS / fs;
    u8 buf[512];
    for (int k = 0; k < frames; k++) {
        int n = sbc_encode(&s, in + (size_t)k * fs * 2, buf);
        if (n != flen) { CHECK(0, "frame %d is %d bytes", k, n); break; }
        if (k == 0) CHECK(buf[0] == 0x9c && buf[1] == 0xb9 && buf[2] == 53, "header %02x %02x %02x", buf[0], buf[1], buf[2]);
        fwrite(buf, 1, (size_t)n, f);
    }
    fclose(f);
    const char *ff = getenv("FFMPEG");
    if (!ff) {
        printf("sbc: %d frames of %d bytes (no $FFMPEG: not decoded)%s\n", frames, flen, fails ? "" : " - ok");
        return fails != 0;
    }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s -hide_banner -loglevel error -y -f sbc -i %s -f s16le -ac 2 build/test_sbc.raw", ff, path);
    CHECK(system(cmd) == 0, "ffmpeg could not decode the stream");
    FILE *r = fopen("build/test_sbc.raw", "rb");
    static i16 out[RATE * SECS * 2];
    int got = r ? (int)fread(out, 4, RATE * SECS, r) : 0;
    if (r) fclose(r);
    CHECK(got >= frames * fs - fs, "decoded %d of %d frames", got, frames * fs);
    double pl = pitch(out + 4410 * 2, got - 8820, 2), pr = pitch(out + 4410 * 2 + 1, got - 8820, 2);
    CHECK(fabs(pl - 1000) < 1 && fabs(pr - 440) < 1, "pitch %.2f / %.2f Hz (want 1000 / 440)", pl, pr);
    /* SNR: the decoder's output lags the input by the filter bank's delay */
    double best = -1e9; int lag_best = 0;
    for (int lag = 0; lag < 400; lag++) {
        double sig = 0, err = 0;
        for (int i = 4410; i < got - 4410; i++)
            for (int ch = 0; ch < 2; ch++) {
                double a = in[2 * (i - lag) + ch], b = out[2 * i + ch];
                sig += a * a; err += (a - b) * (a - b);
            }
        double snr = 10 * log10(sig / (err + 1e-9));
        if (snr > best) { best = snr; lag_best = lag; }
    }
    CHECK(best > 30, "SNR %.1f dB at a delay of %d samples (want > 30 dB)", best, lag_best);
    if (!fails) printf("sbc: %d frames decode (ffmpeg): 1000 / 440 Hz, SNR %.1f dB, delay %d samples - ok\n", frames, best, lag_best);
    return fails != 0;
}
