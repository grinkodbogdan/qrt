/* play.c - plays a WAV file, or a test tone, through /dev/dsp (OSS).
 *
 *   play song.wav        PCM WAV: 8 or 16 bit, mono or stereo, any rate
 *   play -t [hz] [s]     a sine tone (default 440 Hz for 2 s)
 *
 * The kernel mixes it with other sounds and plays it on the current output: USB audio
 * or Bluetooth headphones (Settings -> Sound shows which). */
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SNDCTL_DSP_SYNC     0x00005001
#define SNDCTL_DSP_SPEED    0xc0045002
#define SNDCTL_DSP_SETFMT   0xc0045005
#define SNDCTL_DSP_CHANNELS 0xc0045006
#define SNDCTL_DSP_GETOPTR  0x800c5012
#define AFMT_S16_LE         0x10

static int dsp_open(int rate, int channels) {
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) { perror("play: /dev/dsp"); exit(1); }
    int fmt = AFMT_S16_LE, ch = channels, r = rate;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) || ioctl(fd, SNDCTL_DSP_SPEED, &r)) {
        perror("play: setting the format");
        exit(1);
    }
    return fd;
}

static void finish(int fd, long bytes) {
    ioctl(fd, SNDCTL_DSP_SYNC, 0);                         /* wait until it has been heard */
    int info[3] = { 0 };
    ioctl(fd, SNDCTL_DSP_GETOPTR, info);
    printf("play: %ld bytes written, %d played\n", bytes, info[0]);
    close(fd);
}

static int tone(double hz, double secs) {
    int rate = 48000, n = (int)(rate * secs);
    int fd = dsp_open(rate, 2);
    printf("play: %.0f Hz for %.1f s\n", hz, secs);
    static int16_t buf[2 * 4800];
    long total = 0;
    for (int i = 0; i < n;) {
        int m = n - i < 4800 ? n - i : 4800;
        for (int j = 0; j < m; j++) {
            double t = (double)(i + j) / rate;
            double env = (i + j) < 480 ? (i + j) / 480.0 : (n - i - j) < 480 ? (n - i - j) / 480.0 : 1;   /* no clicks */
            int16_t v = (int16_t)(12000 * env * sin(2 * M_PI * hz * t));
            buf[2 * j] = buf[2 * j + 1] = v;
        }
        if (write(fd, buf, (size_t)m * 4) != m * 4) { perror("play: write"); return 1; }
        total += m * 4;
        i += m;
    }
    finish(fd, total);
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fprintf(stderr, "play: %s is not a WAV file\n", path); return 1; }
    int rate = 0, channels = 0, bits = 0, format = 0;
    uint32_t data = 0;
    for (;;) {                                            /* chunks: fmt, then data */
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) { fprintf(stderr, "play: no data chunk\n"); return 1; }
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[40] = { 0 };
            if (len > sizeof fm || fread(fm, 1, len, f) != len) { fprintf(stderr, "play: bad fmt chunk\n"); return 1; }
            format = le16(fm); channels = le16(fm + 2); rate = (int)le32(fm + 4); bits = le16(fm + 14);
            if (format == 0xfffe && len >= 26) format = le16(fm + 24);     /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(c, "data", 4)) { data = len; break; }
        else fseek(f, (long)(len + (len & 1)), SEEK_CUR);
    }
    if (format != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2) {
        fprintf(stderr, "play: only PCM WAV, 8 or 16 bit, mono or stereo (this one: format %d, %d bit, %d channels)\n", format, bits, channels);
        return 1;
    }
    printf("play: %s - %d Hz, %d bit, %s, %.1f s\n", path, rate, bits, channels == 2 ? "stereo" : "mono",
           (double)data / (rate * channels * bits / 8));
    int fd = dsp_open(rate, channels);
    static uint8_t in[16384];
    static int16_t out[16384];
    long total = 0;
    while (data) {
        size_t want = data < sizeof in ? data : sizeof in;
        size_t got = fread(in, 1, want, f);
        if (!got) break;
        data -= (uint32_t)got;
        size_t bytes;
        if (bits == 8) { for (size_t i = 0; i < got; i++) out[i] = (int16_t)((in[i] - 128) << 8); bytes = got * 2; }
        else { memcpy(out, in, got & ~(size_t)1); bytes = got & ~(size_t)1; }
        if (write(fd, out, bytes) != (ssize_t)bytes) { perror("play: write"); return 1; }
        total += (long)bytes;
    }
    fclose(f);
    finish(fd, total);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "-t"))
        return tone(argc > 2 ? atof(argv[2]) : 440, argc > 3 ? atof(argv[3]) : 2);
    if (argc != 2) { fprintf(stderr, "usage: play file.wav | play -t [hz] [seconds]\n"); return 2; }
    return wav(argv[1]);
}
