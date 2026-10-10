/*
 * lsound.c - sound through Linux's own drivers: on the Xiaomi Mi A1 the audio DSP (the
 * ADSP, Qualcomm's QDSP6) mixes and sends to the speaker amplifier (MAX98927, on the
 * quinary MI2S bus) and the headphone codec (PM8953's WCD).  Linux's qdsp6 drivers and
 * the msm8953 sound card (from postmarketOS's msm8953 tree) make that an ALSA card; this
 * file
 *   - starts the DSP: its firmware comes from the modem partition, it is started through
 *     the secure world (as Wi-Fi's core is);
 *   - sets the card's routing as postmarketOS's UCM does for this phone (Xiaomi/daisy
 *     HiFi.conf, which tissot uses): MultiMedia3 to the speaker;
 *   - plays QRT's mix (sound.c) on the speaker's PCM, hw:0,2, 48 kHz stereo 16-bit -
 *     and stops the stream after a few seconds of silence, so the DSP and amplifier rest.
 * Raw ALSA ioctls - there is no alsa-lib in the kernel.  Layouts from the built Linux's
 * include/uapi/sound/asound.h (checked on the host).
 */
#include "arm.h"
#include "sched.h"
#include "../../kernel/sound.h"

#ifdef QRT_LKL
long argon_sys(long nr, long a, long b, long c, long d, long e);
const char *argon_part_path(const char *part);
int argon_remoteproc_start(const char *fw);
int linux_running(void);

#define NR_IOCTL      29
#define NR_OPENAT     56
#define NR_CLOSE      57
#define NR_READ       63
#define AT_FDCWD      (-100)
#define S(nr, a, b, c, d, e) argon_sys(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))
#define LOG(...) klog("sound: " __VA_ARGS__)

/* ALSA */
#define PCM_HW_PARAMS   0xc2604111u
#define PCM_PREPARE     0x4140u
#define PCM_DROP        0x4143u
#define PCM_WRITEI      0x40184150u
#define CTL_ELEM_LIST   0xc0505510u
#define CTL_ELEM_INFO   0xc1105511u
#define CTL_ELEM_WRITE  0xc4c85513u
#define EPIPE 32

static char state[96] = "starting: Linux and the audio DSP";
static int card_up;
static int testing;                                  /* qrt.soundtest (QEMU): Linux's dummy card, no DSP */
const char *lsound_status(void) { return state; }

static int rd(const char *path, char *out, int cap) {
    long fd = S(NR_OPENAT, AT_FDCWD, path, 0, 0, 0);
    if (fd < 0) return -1;
    long n = S(NR_READ, fd, out, cap - 1, 0, 0);
    S(NR_CLOSE, fd, 0, 0, 0, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return (int)n;
}

/* ---- the card's controls: set one by its name ---- */
static u8 ids[512 * 64];                             /* struct snd_ctl_elem_id, 64 bytes each */
static int nids;
static int ctl_list(long fd) {
    u8 l[80];
    memset(l, 0, sizeof l);
    *(u32 *)(l + 4) = 512;                           /* space */
    *(u64 *)(l + 16) = (u64)(usize)ids;              /* pids */
    if (S(NR_IOCTL, fd, CTL_ELEM_LIST, l, 0, 0) < 0) return -1;
    nids = (int)*(u32 *)(l + 8);                     /* used */
    return nids;
}
static const u8 *ctl_find(const char *name) {
    for (int i = 0; i < nids; i++) if (!strcmp((const char *)ids + i * 64 + 16, name)) return ids + i * 64;
    return NULL;
}
/* value: integers and switches; an enumerated control by its item's name (text) */
static int ctl_set(long fd, const char *name, long value, const char *text) {
    const u8 *id = ctl_find(name);
    if (!id) { LOG("no control '%s'", name); return -1; }
    static u8 info[272], val[1224];
    memset(info, 0, sizeof info);
    memcpy(info, id, 64);
    if (S(NR_IOCTL, fd, CTL_ELEM_INFO, info, 0, 0) < 0) return -1;
    u32 type = *(u32 *)(info + 64), count = *(u32 *)(info + 72);
    if (count > 128) count = 128;
    memset(val, 0, sizeof val);
    memcpy(val, id, 64);
    if (type == 3) {                                 /* ENUMERATED */
        u32 items = *(u32 *)(info + 80), pick = (u32)value;
        for (u32 i = 0; text && i < items; i++) {
            *(u32 *)(info + 84) = i;
            if (S(NR_IOCTL, fd, CTL_ELEM_INFO, info, 0, 0) == 0 && !strcmp((const char *)info + 88, text)) { pick = i; break; }
        }
        for (u32 c = 0; c < count; c++) *(u32 *)(val + 72 + 4 * c) = pick;
    } else for (u32 c = 0; c < count; c++) *(i64 *)(val + 72 + 8 * c) = value;   /* BOOLEAN, INTEGER */
    long r = S(NR_IOCTL, fd, CTL_ELEM_WRITE, val, 0, 0);
    if (r < 0) LOG("'%s' refused (%ld)", name, r);
    return r < 0 ? -1 : 0;
}

/* ---- the PCM ---- */
static void interval(u8 *hp, int param, u32 min, u32 max) {
    u8 *iv = hp + 260 + 12 * (param - 8);
    *(u32 *)iv = min; *(u32 *)(iv + 4) = max; *(u32 *)(iv + 8) = 1u << 2;   /* integer */
}
static int pcm_setup(long fd, int period, int periods, int *got_period) {
    static u8 hp[608];
    memset(hp, 0, sizeof hp);
    for (int m = 0; m < 3; m++) memset(hp + 4 + 32 * m, 0xff, 32);
    for (int i = 0; i < 12; i++) { u8 *iv = hp + 260 + 12 * i; *(u32 *)(iv + 4) = 0xffffffffu; }
    memset(hp + 4, 0, 32); hp[4] = 1u << 3;            /* ACCESS: RW_INTERLEAVED */
    memset(hp + 36, 0, 32); hp[36] = 1u << 2;          /* FORMAT: S16_LE */
    memset(hp + 68, 0, 32); hp[68] = 1;                /* SUBFORMAT: STD */
    interval(hp, 10, 2, 2);                            /* CHANNELS */
    interval(hp, 11, 48000, 48000);                    /* RATE */
    if (period) interval(hp, 13, (u32)period, (u32)period);     /* PERIOD_SIZE */
    if (periods) interval(hp, 15, (u32)periods, (u32)periods);  /* PERIODS */
    *(u32 *)(hp + 512) = 0xffffffffu;                  /* rmask */
    *(u32 *)(hp + 520) = 0xffffffffu;                  /* info */
    long r = S(NR_IOCTL, fd, PCM_HW_PARAMS, hp, 0, 0);
    if (r < 0) return (int)r;
    *got_period = (int)*(u32 *)(hp + 260 + 12 * (13 - 8));
    return 0;
}

static snd_output_t out = { .name = "Speaker", .rate = 48000, .channels = 2 };

static void sound_loop(void *a) {
    (void)a;
    /* the DSP: its firmware is on the modem partition (mounted by linux.c) */
    for (int t = 0; t < 240 && !linux_running(); t++) thr_sleep_us(500000);
    const char *pcm = testing ? "/dev/snd/pcmC0D0p" : "/dev/snd/pcmC0D2p";
    if (testing) goto card;
    for (int t = 0; t < 120 && !argon_part_path("modem_a") && !argon_part_path("modem"); t++) thr_sleep_us(500000);
    if (!argon_part_path("modem_a") && !argon_part_path("modem")) {
        fmt(state, sizeof state, "off: the modem partition (the DSP's firmware) is not mounted");
        LOG("%s", state);
        goto silent;
    }
    /* starting it goes through the secure world; if that ever resets the phone, the next
     * boot leaves sound off (and says so) instead of resetting again */
    if (plog_prev_state() == PLOG_DSP_STARTING) {
        fmt(state, sizeof state, "off this boot: the last boot reset while the audio DSP started");
        LOG("%s", state);
        goto silent;
    }
    plog_state(PLOG_DSP_STARTING);
    int r = argon_remoteproc_start("adsp");
    plog_state(PLOG_LINUX_OK);
    if (r <= 0) {
        fmt(state, sizeof state, "off: the audio DSP %s", r ? "did not start" : "is not in Linux");
        LOG("%s", state);
        goto silent;
    }
card:;
    /* the card: the DSP's services come up over SMD, then the sound card binds */
    char t[64], sp[64];
    fmt(sp, sizeof sp, "/sys/class/sound/%s/dev", pcm + 9);
    int i = 0;
    for (; i < 150 && rd(sp, t, sizeof t) <= 0; i++) {
        thr_sleep_us(200000);
        if (!testing && (i == 50 || i == 100)) { int argon_sound_reprobe(int); argon_sound_reprobe(0); }
    }
    if (i == 150) {
        /* say why, then keep waiting: a card can bind late (its parts probe in any order) */
        void argon_sound_report(void);
        fmt(state, sizeof state, "waiting: no sound card 30 s after the DSP started (see the log)");
        LOG("no sound card 30 s after the DSP started - what Linux has:");
        argon_sound_report();
        int argon_sound_reprobe(int report);
        for (i = 0; i < 900 && rd(sp, t, sizeof t) <= 0; i++) {
            thr_sleep_us(200000);
            if (i % 50 == 25) argon_sound_reprobe(0);                /* every 10 s: try the codecs again */
            if (i == 450) { LOG("still no sound card at 2 minutes:"); argon_sound_report(); }
        }
        if (i == 900) {
            fmt(state, sizeof state, "off: no sound card (see the log's sound: lines)");
            LOG("%s", state);
            goto silent;
        }
        LOG("the sound card came late");
    }
    S(34, AT_FDCWD, "/proc", 0555, 0, 0);                            /* mkdirat; mount proc */
    S(40, "proc", "/proc", "proc", 0, 0);
    if (rd("/proc/asound/cards", t, sizeof t) <= 0) t[0] = 0;
    for (char *c = t; *c; c++) if (*c == '\n') *c = ' ';
    LOG("card: %s", t);
    long cfd = S(NR_OPENAT, AT_FDCWD, "/dev/snd/controlC0", 2, 0, 0);
    if (cfd >= 0 && ctl_list(cfd) > 0) {
        LOG("%d controls", nids);
        if (testing) {
            for (int k = 0; k < nids && k < 4; k++) LOG("test: control '%s'", (const char *)ids + k * 64 + 16);
            if (ctl_set(cfd, "Master Volume", 42, NULL) == 0) LOG("test: a control was set");
        } else ctl_set(cfd, "QUIN_MI2S_RX Audio Mixer MultiMedia3", 1, NULL);   /* the speaker */
        S(NR_CLOSE, cfd, 0, 0, 0, 0);
    } else LOG("the card's controls did not open (%ld)", cfd);

    long fd = S(NR_OPENAT, AT_FDCWD, pcm, 1, 0, 0);                 /* O_WRONLY */
    if (fd < 0) { fmt(state, sizeof state, "off: the speaker's PCM did not open (%ld)", fd); LOG("%s", state); goto silent; }
    int period = 0;
    if ((r = pcm_setup(fd, 960, 4, &period)) < 0 && (r = pcm_setup(fd, 0, 0, &period)) < 0) {
        fmt(state, sizeof state, "off: the speaker refused 48 kHz stereo (%d)", r);
        LOG("%s", state);
        goto silent;
    }
    if (period <= 0 || period > 4096) period = 960;
    fmt(state, sizeof state, "built-in speaker (the audio DSP), 48 kHz");
    LOG("speaker ready: 48 kHz stereo, %d frames a period", period);
    card_up = 1;
    snd_output_add(&out);
    if (testing) snd_beep();                                        /* something to play */
    static i16 buf[2 * 4096];
    int playing = 0;
    u64 quiet_since = k_now_ms();
    for (;;) {
        snd_mix(buf, period, 48000, 2);
        int loud = 0;
        for (int k = 0; k < 2 * period && !loud; k++) if (buf[k] > 8 || buf[k] < -8) loud = 1;
        u64 now = k_now_ms();
        if (loud) quiet_since = now;
        if (!playing) {
            if (!loud) { thr_sleep_us((u64)period * 1000000 / 48000); continue; }   /* real time, into nothing */
            S(NR_IOCTL, fd, PCM_PREPARE, 0, 0, 0);
            playing = 1;
            static int starts;
            if (starts++ < 3) LOG("playing");
        }
        struct { long result; void *buf; unsigned long frames; } x = { 0, buf, (unsigned long)period };
        long w = S(NR_IOCTL, fd, PCM_WRITEI, &x, 0, 0);
        if (w == -EPIPE) { S(NR_IOCTL, fd, PCM_PREPARE, 0, 0, 0); S(NR_IOCTL, fd, PCM_WRITEI, &x, 0, 0); }
        else if (w < 0) {
            static int said;
            if (said++ < 3) LOG("write refused (%ld)", w);
            thr_sleep_us(20000);
        }
        else if (playing == 1) {                    /* the first period went out */
            playing = 2;
            static int said_ok;
            if (said_ok++ < 3) LOG("the speaker takes the sound (%ld)", x.result);
        }
        if (playing && now - quiet_since > 3000) { S(NR_IOCTL, fd, PCM_DROP, 0, 0, 0); playing = 0; static int qs; if (qs++ < 3) LOG("quiet: stream stopped"); }   /* silence: rest */
    }
silent:
    for (;;) { snd_tick(k_now_us()); thr_sleep_us(10000); }       /* players keep time */
}

static void sound_loop_none(void *a) {
    (void)a;
    for (;;) { snd_tick(k_now_us()); thr_sleep_us(10000); }       /* no output: players keep time */
}
void lsound_start(void) {
    int ch = fdt_node("/chosen"), al;
    const char *args = ch >= 0 ? fdt_prop(ch, "bootargs", &al) : NULL;
    testing = args && strstr(args, "qrt.soundtest");
    if ((!testing && fdt_find_compatible(-1, "qcom,msm8953-qdsp6-sndcard") < 0) || (args && strstr(args, "qrt.nosound"))) {
        fmt(state, sizeof state, "no sound driver for this device yet");
        thr_create("sound", sound_loop_none, NULL, 16 << 10);
        return;
    }
    thr_create("sound", sound_loop, NULL, 64 << 10);
}
#else
const char *lsound_status(void) { return "no sound driver on ARM yet"; }
void lsound_start(void) {}
#endif
