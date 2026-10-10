/*
 * lgpu.c - the Mi A1's GPU (Adreno 506) through Linux's msm driver - first step: bring
 * it up and see it run.
 *
 * At boot the GPU and its IOMMU are kept from Linux (main.c): probing them once faulted
 * on the secure world.  After Linux and the phone's partitions are up, this thread
 *   - copies the GPU's firmware from the vendor partition (the CP microcode a530_pm4.fw
 *     and a530_pfp.fw, the "zap" shader a506_zap.* the secure world loads),
 *   - gives the IOMMU (msm-iommu-v2 at 1c48000) back to Linux and has it probed, then the
 *     GPU - a headless DRM device (QRT patch: the display stays QRT's),
 *   - opens it: Linux loads the firmware, the secure world takes the zap shader, and the
 *     command processor must run its init packets (CP_ME_INIT) and go idle,
 *   - reads the GPU's ID and its always-on timestamp twice: counting means it runs.
 * A reset while doing this leaves the GPU alone at the next boot (as the audio DSP).
 * Drawing with it - command streams of QRT's own, as on the tablets' Intel GPU - is the
 * next step.
 */
#include "arm.h"
#include "sched.h"

#ifdef QRT_LKL
long argon_sys(long nr, long a, long b, long c, long d, long e);
const char *argon_part_path(const char *part);
int linux_running(void);
int arm_unhide(const char *hidden_compat);

#define NR_IOCTL      29
#define NR_MKDIRAT    34
#define NR_OPENAT     56
#define NR_CLOSE      57
#define NR_GETDENTS64 61
#define NR_READ       63
#define NR_WRITE      64
#define AT_FDCWD      (-100)
#define S(nr, a, b, c, d, e) argon_sys(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))
#define LOG(...) klog("gpu: " __VA_ARGS__)

#define MSM_GET_PARAM 0xc0186440u
enum { PIPE_3D0 = 0x10, P_GPU_ID = 1, P_GMEM = 2, P_CHIP_ID = 3, P_TIMESTAMP = 5 };

static char state[96] = "off";
const char *gpu_status(void) { return state; }

static int rd(const char *path, char *out, int cap) {
    long f = S(NR_OPENAT, AT_FDCWD, path, 0, 0, 0);
    if (f < 0) return -1;
    long n = S(NR_READ, f, out, cap - 1, 0, 0);
    S(NR_CLOSE, f, 0, 0, 0, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return (int)n;
}
static void wr(const char *path, const char *t) {
    long f = S(NR_OPENAT, AT_FDCWD, path, 1, 0, 0);
    if (f < 0) return;
    S(NR_WRITE, f, t, strlen(t), 0, 0);
    S(NR_CLOSE, f, 0, 0, 0, 0);
}
static long copy(const char *from, const char *to) {
    long in = S(NR_OPENAT, AT_FDCWD, from, 0, 0, 0);
    if (in < 0) return -1;
    long out = S(NR_OPENAT, AT_FDCWD, to, 01101, 0644, 0);
    if (out < 0) { S(NR_CLOSE, in, 0, 0, 0, 0); return -1; }
    static u8 b[16384];
    long n, tot = 0;
    while ((n = S(NR_READ, in, b, sizeof b, 0, 0)) > 0) { S(NR_WRITE, out, b, n, 0, 0); tot += n; }
    S(NR_CLOSE, in, 0, 0, 0, 0);
    S(NR_CLOSE, out, 0, 0, 0, 0);
    return tot;
}

/* the GPU's files from <vendor>/firmware: a530_* to qcom/, a506_zap.* also where the tree
 * says (qcom/msm8953/xiaomi/tissot/) */
static int firmware(void) {
    static const char *const parts[] = { "vendor_a", "vendor", "vendor_b", "system_a", NULL };
    static const char *const dirs[] = { "firmware", "vendor/firmware", "etc/firmware", NULL };
    S(NR_MKDIRAT, AT_FDCWD, "/lib", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware/qcom", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware/qcom/msm8953", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware/qcom/msm8953/xiaomi", 0755, 0, 0);
    S(NR_MKDIRAT, AT_FDCWD, "/lib/firmware/qcom/msm8953/xiaomi/tissot", 0755, 0, 0);
    int got = 0, zap = 0;
    for (int p = 0; parts[p] && got < 2; p++) {
        const char *m = argon_part_path(parts[p]);
        if (!m) continue;
        for (int d = 0; dirs[d]; d++) {
            char dir[128];
            fmt(dir, sizeof dir, "%s/%s", m, dirs[d]);
            long fd = S(NR_OPENAT, AT_FDCWD, dir, 0200000, 0, 0);
            if (fd < 0) continue;
            static char ents[8192];
            long n;
            while ((n = S(NR_GETDENTS64, fd, ents, sizeof ents, 0, 0)) > 0)
                for (long o = 0; o < n; o += *(u16 *)(ents + o + 16)) {
                    const char *nm = ents + o + 19;
                    int pm = !strcmp(nm, "a530_pm4.fw") || !strcmp(nm, "a530_pfp.fw");
                    int z = !strncmp(nm, "a506_zap.", 9);
                    if (!pm && !z) continue;
                    char from[192], to[192];
                    fmt(from, sizeof from, "%s/%s", dir, nm);
                    fmt(to, sizeof to, "/lib/firmware/qcom/%s", nm);
                    long b = copy(from, to);
                    if (z) { fmt(to, sizeof to, "/lib/firmware/qcom/msm8953/xiaomi/tissot/%s", nm); copy(from, to); zap++; }
                    if (pm && b > 0) got++;
                    if (b > 0) LOG("firmware %s (%ld bytes, from %s/%s)", nm, b, parts[p], dirs[d]);
                }
            S(NR_CLOSE, fd, 0, 0, 0, 0);
        }
    }
    if (got < 2) LOG("the CP microcode (a530_pm4.fw, a530_pfp.fw) was not found on the vendor partition");
    if (!zap) LOG("the zap shader (a506_zap.*) was not found: the GPU may not leave secure mode");
    return got >= 2;
}

static int bound(const char *dev) {
    char p[96], t[8];
    fmt(p, sizeof p, "/sys/bus/platform/devices/%s/driver/uevent", dev);
    return rd(p, t, sizeof t) >= 0;
}
static int probe(const char *compat_hidden, const char *dev, int secs) {
    if (!arm_unhide(compat_hidden)) { LOG("no %s in the tree", compat_hidden); return 0; }
    for (int t = 0; t < secs * 5 && !bound(dev); t++) {
        if (t % 10 == 0) wr("/sys/bus/platform/drivers_probe", dev);
        thr_sleep_us(200000);
    }
    return bound(dev);
}

/* what Linux said about it (its own log: the console may not carry it) */
static void report(void) {
    static char kb[65536];
    long n = S(116, 3, kb, sizeof kb - 1, 0, 0);                     /* syslog: SYSLOG_ACTION_READ_ALL */
    kb[n > 0 ? n : 0] = 0;
    static const char *lines[30];
    int nl = 0;
    for (char *l = kb; *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        static const char *const w[] = { "msm", "adreno", "a5xx", "iommu", "zap", "gpu", "a530", "1c00000", "1c48000", NULL };
        int hit = 0;
        if (!strstr(l, "initcall") && !strstr(l, "calling ")) for (int i = 0; w[i] && !hit; i++) hit = strstr(l, w[i]) != NULL;
        if (hit) lines[nl++ % 30] = l;
        if (!e) break;
        l = e + 1;
    }
    for (int i = nl > 30 ? nl - 30 : 0; i < nl; i++) {
        const char *m = lines[i % 30];
        if (*m == '<' && strchr(m, '>')) m = strchr(m, '>') + 1;
        if (*m == '[' && strchr(m, ']')) m = strchr(m, ']') + 2;
        LOG("linux said: %s", m);
    }
    if (!nl) LOG("Linux said nothing about the GPU or its IOMMU");
}

static long param(long fd, u32 p, u64 *v) {
    u8 a[24];
    memset(a, 0, sizeof a);
    *(u32 *)a = PIPE_3D0;
    *(u32 *)(a + 4) = p;
    long r = S(NR_IOCTL, fd, MSM_GET_PARAM, a, 0, 0);
    *v = *(u64 *)(a + 8);
    return r;
}

static void gpu_loop(void *a) {
    (void)a;
    for (int t = 0; t < 240 && !linux_running(); t++) thr_sleep_us(500000);
    for (int t = 0; t < 120 && !argon_part_path("vendor_a") && !argon_part_path("vendor"); t++) thr_sleep_us(500000);
    thr_sleep_us(5000000);                                           /* after Wi-Fi's and sound's starts */
    if (plog_prev_state() == PLOG_GPU_STARTING) {
        fmt(state, sizeof state, "off this boot: the last boot reset while it started");
        LOG("%s", state);
        return;
    }
    fmt(state, sizeof state, "starting");
    if (!firmware()) { fmt(state, sizeof state, "off: no firmware on the vendor partition"); return; }
    plog_state(PLOG_GPU_STARTING);
    if (!probe("qrt-,msm-iommu-v2", "1c48000.iommu", 10)) {
        plog_state(PLOG_LINUX_OK);
        fmt(state, sizeof state, "off: its IOMMU did not come up (see the log)");
        LOG("%s", state);
        report();
        return;
    }
    LOG("IOMMU (msm-iommu-v2) up");
    if (!probe("qrt-,adreno", "1c00000.gpu", 10)) {
        plog_state(PLOG_LINUX_OK);
        fmt(state, sizeof state, "off: Linux did not take the GPU (see the log)");
        LOG("%s", state);
        report();
        return;
    }
    long fd = -1;
    for (int t = 0; t < 50 && fd < 0; t++) { fd = S(NR_OPENAT, AT_FDCWD, "/dev/dri/card0", 2, 0, 0); if (fd < 0) thr_sleep_us(200000); }
    if (fd < 0) { plog_state(PLOG_LINUX_OK); fmt(state, sizeof state, "off: no /dev/dri/card0 (%ld)", fd); LOG("%s", state); report(); return; }
    u64 id = 0, chip = 0, gmem = 0, t0 = 0, t1 = 0;
    long r = param(fd, P_GPU_ID, &id);                               /* the first use: firmware, zap, CP init */
    param(fd, P_CHIP_ID, &chip);
    param(fd, P_GMEM, &gmem);
    long r0 = param(fd, P_TIMESTAMP, &t0);
    thr_sleep_us(100000);
    long r1 = param(fd, P_TIMESTAMP, &t1);
    plog_state(PLOG_LINUX_OK);
    if (r < 0 || !id) { fmt(state, sizeof state, "off: the GPU did not start (%ld)", r); LOG("%s", state); report(); return; }
    if (r0 < 0 || r1 < 0 || t1 <= t0) {
        fmt(state, sizeof state, "Adreno %llu present, not running (timestamp %llu, %llu)", id, t0, t1);
        LOG("%s", state);
        report();
        return;
    }
    fmt(state, sizeof state, "Adreno %llu running (chip %llx, %llu KB GMEM)", id, chip, gmem >> 10);
    LOG("%s; its always-on counter moved %llu in 100 ms", state, t1 - t0);
}

void lgpu_start(void) {
    int ch = fdt_node("/chosen"), al;
    const char *args = ch >= 0 ? fdt_prop(ch, "bootargs", &al) : NULL;
    if (fdt_find_compatible(-1, "qrt-,adreno") < 0 || (args && strstr(args, "qrt.nogpu"))) return;
    thr_create("gpu", gpu_loop, NULL, 32 << 10);
}
#else
const char *gpu_status(void) { return "the boot loader's framebuffer"; }
void lgpu_start(void) {}
#endif
