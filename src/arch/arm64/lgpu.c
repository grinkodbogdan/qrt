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
 *
 * Then QRT's own command streams (0.27.0), as on the tablets' Intel GPU: buffers come
 * from Linux's driver (GEM_NEW), QRT writes packets into them directly (a QRT patch hands
 * out their kernel address - QRT shares Linux's address space) and submits them
 * (GEM_SUBMIT, WAIT_FENCE).  Two checks first - a CP_MEM_WRITE the CPU reads back, then a
 * 64 x 64 copy by the 2D engine compared pixel by pixel - and only then does the GPU put
 * frames on the screen: the display pipe moves to a 32-bit buffer (fb.c; the engine's
 * pitches are 64-byte units, the splash buffer's 24-bit rows 3240 bytes), the shell's
 * frame and that buffer are mapped for the GPU (QRT patch: QRT_MAP_PHYS) and each changed
 * rectangle is one CP_BLIT - the CPU only writes back its cache over the rectangle.
 * The 2D engine's setup follows Mesa's freedreno (fd5_blitter.c: emit_setup, emit_blit).
 */
#include "arm.h"
#include "sched.h"
#include "../../kernel/smp.h"

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

/* a driver took it: its "driver" link leads to a directory (0.26.0 read the driver's
 * uevent, which is write-only - every probe looked failed) */
static int bound(const char *dev) {
    char p[96];
    fmt(p, sizeof p, "/sys/bus/platform/devices/%s/driver", dev);
    long f = S(NR_OPENAT, AT_FDCWD, p, 0200000, 0, 0);               /* O_DIRECTORY */
    if (f < 0) return 0;
    S(NR_CLOSE, f, 0, 0, 0, 0);
    return 1;
}
/* a platform device's name by its address: a node with a "reg" is "1c00000.gpu", one with
 * only "ranges" (the IOMMU) is named after its path - 0.26.1 looked for "1c48000.iommu" */
static char iommu_dev[96] = "1c48000.iommu";
static void find_dev(const char *addr, char *out, usize cap) {
    long fd = S(NR_OPENAT, AT_FDCWD, "/sys/bus/platform/devices", 0200000, 0, 0);
    if (fd < 0) return;
    static char ents[8192];
    long n;
    char best[96] = "";
    while ((n = S(NR_GETDENTS64, fd, ents, sizeof ents, 0, 0)) > 0)
        for (long o = 0; o < n; o += *(u16 *)(ents + o + 16)) {
            const char *nm = ents + o + 19;
            if (!strstr(nm, addr) || strstr(nm, "ctx")) continue;       /* the IOMMU, not its context banks */
            if (!best[0] || strlen(nm) < strlen(best)) strlcpy(best, nm, sizeof best);
        }
    S(NR_CLOSE, fd, 0, 0, 0, 0);
    if (best[0]) strlcpy(out, best, cap);
}
static int probe(const char *compat_hidden, const char *dev, int secs) {
    if (!arm_unhide(compat_hidden)) { LOG("no %s in the tree", compat_hidden); return 0; }
    for (int t = 0; t < secs * 5 && !bound(dev); t++) {
        if (t % 10 == 0) wr("/sys/bus/platform/drivers_probe", dev);
        thr_sleep_us(200000);
    }
    return bound(dev);
}

static int bound(const char *dev);
/* what Linux said about it (its own log: the console may not carry it) */
static void report(void) {
    static char kb[65536];
    long n = S(116, 3, kb, sizeof kb - 1, 0, 0);                     /* syslog: SYSLOG_ACTION_READ_ALL */
    kb[n > 0 ? n : 0] = 0;
    static const char *lines[40];                                  /* backtrace frames left out */
    int nl = 0;
    for (char *l = kb; *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        static const char *const w[] = { "msm", "adreno", "a5xx", "iommu", "zap", "gpu", "a530", "1c00000", "1c48000", "drm", "VRAM", "gdsc", "opp", NULL };
        int hit = 0;
        if (!strstr(l, "initcall") && !strstr(l, "calling ") && !strstr(l, "[<0x") && !strstr(l, "Got CD GPIO")) for (int i = 0; w[i] && !hit; i++) hit = strstr(l, w[i]) != NULL;
        if (hit) lines[nl++ % 40] = l;
        if (!e) break;
        l = e + 1;
    }
    for (int i = nl > 40 ? nl - 40 : 0; i < nl; i++) {
        const char *m = lines[i % 40];
        if (*m == '<' && strchr(m, '>')) m = strchr(m, '>') + 1;
        if (*m == '[' && strchr(m, ']')) m = strchr(m, ']') + 2;
        LOG("linux said: %s", m);
    }
    if (!nl) LOG("Linux said nothing about the GPU or its IOMMU");
    const char *const devs[] = { iommu_dev, "1c00000.gpu", "msm", NULL };   /* msm: the headless DRM device */
    for (int i = 0; devs[i]; i++) {
        char p[96];
        fmt(p, sizeof p, "/sys/bus/platform/devices/%s", devs[i]);
        long f = S(NR_OPENAT, AT_FDCWD, p, 0200000, 0, 0);
        if (f >= 0) S(NR_CLOSE, f, 0, 0, 0, 0);
        LOG("device %s: %s", devs[i], f < 0 ? "not in Linux" : bound(devs[i]) ? "has its driver" : "no driver");
    }
    long f = S(NR_OPENAT, AT_FDCWD, "/sys/kernel/debug/devices_deferred", 0, 0, 0);   /* still waiting, and why */
    if (f >= 0) {
        static char dd[4096];
        long n2 = S(NR_READ, f, dd, sizeof dd - 1, 0, 0);
        S(NR_CLOSE, f, 0, 0, 0, 0);
        dd[n2 > 0 ? n2 : 0] = 0;
        for (char *l = dd; *l; ) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            if (strstr(l, "1c48000") || strstr(l, "1c00000") || strstr(l, "msm")) { for (char *t = l; *t; t++) if (*t == '\t') *t = ' '; LOG("waiting: %s", l); }
            if (!e) break;
            l = e + 1;
        }
    }
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

static void own_commands(long fd);
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
    find_dev("1c48000", iommu_dev, sizeof iommu_dev);
    LOG("the IOMMU is %s in Linux", iommu_dev);
    if (!probe("qrt-,msm-iommu-v2", iommu_dev, 10)) {
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
    own_commands(fd);
}

/* ---- QRT's command streams ---- */
#define MSM_GEM_NEW    0xc0106442u
#define MSM_GEM_INFO   0xc0186443u
#define MSM_GEM_SUBMIT 0xc0486446u
#define MSM_WAIT_FENCE 0x40206447u
#define BO_WC          0x00020000u
#define INFO_IOVA      1
#define INFO_VADDR     0x5100u           /* QRT patch */
#define INFO_MAP_PHYS  0x5101u           /* QRT patch */

struct bo { u32 handle; u64 iova; u32 *cpu; u32 size; };
static long gfd = -1;
static struct bo cmd, scr;
static volatile int g_on;                /* the GPU puts frames on the screen */
static u64 fb_iova;
static u32 fb_pitch;
static u32 g_frames, g_us;               /* since the last gpu_stats() */
#define NSLOT 8
#define SLOT_BYTES 8192                  /* NSLOT of them fill cmd (64 KB) */
static u32 slot_fence[NSLOT], nsub;
static u64 st_t0, st_clean, st_submit, st_block;
static u32 st_n, st_behind, last_fence;
static int behind;

static long info(u32 handle, u32 what, u64 *value, u32 len) {
    struct { u32 handle, info; u64 value; u32 len, pad; } a = { handle, what, *value, len, 0 };
    long r = S(NR_IOCTL, gfd, MSM_GEM_INFO, &a, 0, 0);
    *value = a.value;
    return r;
}
static int bo_new(struct bo *b, u32 size) {
    struct { u64 size; u32 flags, handle; } a = { size, BO_WC, 0 };
    long r = S(NR_IOCTL, gfd, MSM_GEM_NEW, &a, 0, 0);
    if (r < 0) { LOG("GEM_NEW (%u bytes): %ld", size, r); return 0; }
    u64 v = 0;
    if ((r = info(a.handle, INFO_IOVA, &v, 0)) < 0) { LOG("its GPU address: %ld", r); return 0; }
    b->iova = v;
    v = 0;
    if ((r = info(a.handle, INFO_VADDR, &v, 0)) < 0) { LOG("its CPU address: %ld", r); return 0; }
    b->cpu = (u32 *)(usize)v;
    b->handle = a.handle;
    b->size = size;
    return 1;
}
/* host memory (identity mapped: virtual == physical) for the GPU; 0 if it failed */
static u64 map_phys(u64 pa, u64 len) {
    u64 v = pa;
    long r = info(cmd.handle, INFO_MAP_PHYS, &v, (u32)len);
    if (r < 0) { LOG("mapping %llx (%llu KB) for the GPU: %ld", pa, len >> 10, r); return 0; }
    return v;
}

/* PM4: type-4 (registers) and type-7 (opcodes) packet headers with their parity bits */
static u32 par(u32 v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; return (0x9669u >> (v & 15)) & 1; }
static u32 pkt4(u32 reg, u32 cnt) { return 0x40000000u | cnt | par(cnt) << 7 | (reg & 0x3ffff) << 8 | par(reg) << 27; }
static u32 pkt7(u32 op, u32 cnt) { return 0x70000000u | cnt | par(cnt) << 15 | (op & 0x7f) << 16 | par(op) << 23; }
enum { CP_WAIT_FOR_IDLE = 0x26, CP_MEM_WRITE = 0x3d, CP_EVENT_WRITE = 0x46, CP_SET_RENDER_MODE = 0x6c, CP_BLIT = 0x2c,
       CP_SKIP_IB2_ENABLE_GLOBAL = 0x1d, CP_SKIP_IB2_ENABLE_LOCAL = 0x23 };
enum { EV_CACHE_FLUSH_TS = 4, EV_PC_CCU_FLUSH_COLOR_TS = 29, EV_LRZ_FLUSH = 38 };
enum { RM_BLIT2DSCALE = 7, RM_END2D = 8 };

static u32 *cp;                          /* where the next dword of the stream goes */
static void out(u32 v) { *cp++ = v; }
static void reg1(u32 reg, u32 v) { out(pkt4(reg, 1)); out(v); }
static void render_mode(u32 m, int len) {   /* fd5_set_render_mode: 5 dwords; the blob's 2D: 1? */
    out(pkt7(CP_SET_RENDER_MODE, (u32)len)); out(m);
    for (int i = 1; i < len; i++) out(0);
}
/* how the 2D engine is started: not documented, and 0.27.0's one guess wrote nothing
 * (the CP took it, nothing hung).  The boot tries the combinations on a test copy and
 * keeps the first that copies exactly. */
struct bv { u8 mode, op, rmlen; u32 r2184; };
static struct bv bvs[16];
static int nbv;
static const struct bv *use_bv;
static void event_ts(u32 ev) {
    out(pkt7(CP_EVENT_WRITE, 4)); out(ev); out((u32)(scr.iova + 64)); out((u32)((scr.iova + 64) >> 32)); out(0);
}
/* a rectangle from one 32-bit surface to another, by the 2D engine: format R8G8B8A8,
 * linear, the same byte order on both sides - a plain copy */
static void blit(const struct bv *v, u64 src, u32 spitch, u64 dst, u32 dpitch, int sx, int sy, int dx, int dy, int w, int h, u32 solid) {
    out(pkt7(CP_EVENT_WRITE, 1)); out(EV_LRZ_FLUSH);
    out(pkt7(CP_SKIP_IB2_ENABLE_GLOBAL, 1)); out(0);
    out(pkt7(CP_SKIP_IB2_ENABLE_LOCAL, 1)); out(0);
    out(pkt7(CP_EVENT_WRITE, 1)); out(25);   /* PC_CCU_INVALIDATE_COLOR */
    reg1(0xe3b0, 3);                     /* PC_POWER_CNTL */
    reg1(0xe4f0, 3);                     /* VFD_POWER_CNTL */
    out(pkt7(CP_WAIT_FOR_IDLE, 0));
    reg1(0x0cc7, 0x10000000);            /* RB_CCU_CNTL: bypass */
    reg1(0xe141, 0x00000008);            /* RB_RENDER_CNTL */
    u32 cntl = v->op ? 0x86000000u : 0;  /* 0x86000000 for a copy, 0 for a fill (a5xx.xml) */
    reg1(0x2100, cntl);                  /* RB_2D_BLIT_CNTL */
    reg1(0x2180, cntl);                  /* GRAS_2D_BLIT_CNTL */
    reg1(0x2184, v->r2184);
    if (!v->op) { out(pkt4(0x2101, 4)); out(solid); out(solid); out(solid); out(solid); }   /* RB_2D_SRC_SOLID_DW0-3 */
    reg1(0xe140, 1u << 17);              /* RB_CNTL: BYPASS */
    reg1(0x0cc6, 0x00000004);            /* RB_MODE_CNTL */
    reg1(0x0ec2, 0x0000000c);            /* SP_MODE_CNTL */
    reg1(0x0f02, 0x00000344);            /* TPL1_MODE_CNTL */
    reg1(0x0e06, 0x00000002);            /* HLSQ_MODE_CNTL */
    reg1(0xe000, 0x00000181);            /* GRAS_CL_CNTL */
    render_mode(v->mode, v->rmlen);
    const u32 fmt = 0x30;                /* RB5_R8G8B8A8_UNORM, TILE5_LINEAR, swap WZYX */
    out(pkt4(0x2107, 9));                /* RB_2D_SRC_INFO, _LO, _HI, _SIZE, 5 more */
    out(fmt); out((u32)src); out((u32)(src >> 32)); out(spitch >> 6);
    out(0); out(0); out(0); out(0); out(0);
    out(pkt4(0x2110, 9));                /* RB_2D_DST_INFO ... */
    out(fmt); out((u32)dst); out((u32)(dst >> 32)); out(dpitch >> 6);
    out(0); out(0); out(0); out(0); out(0);
    out(pkt4(0x2181, 2)); out(fmt); out(fmt);   /* GRAS_2D_SRC_INFO, GRAS_2D_DST_INFO */
    out(pkt7(CP_BLIT, 5));
    out(v->op);                          /* BLIT_OP_FILL 0, _COPY 1, _SCALE 3 */
    out((u32)sx | (u32)sy << 16);
    out((u32)(sx + w - 1) | (u32)(sy + h - 1) << 16);
    out((u32)dx | (u32)dy << 16);
    out((u32)(dx + w - 1) | (u32)(dy + h - 1) << 16);
    render_mode(RM_END2D, v->rmlen);
    out(pkt7(CP_WAIT_FOR_IDLE, 0));
    event_ts(EV_PC_CCU_FLUSH_COLOR_TS);
    event_ts(EV_CACHE_FLUSH_TS);
}
/* run what is in cmd (from its start to cp) and wait for it: 0, or why not.  Every
 * buffer the stream touches is listed (the driver maps a buffer's pages for the GPU
 * when a submit lists it): cmd, the scratch page, and up to two more */
static long submit(u32 off, u32 bytes, const struct bo *x1, const struct bo *x2, u32 *fence) {
    __asm__ volatile("dsb sy" ::: "memory");
    struct { u32 flags, handle; u64 presumed; } bo[4] = { { 1, cmd.handle, 0 }, { 3, scr.handle, 0 } };   /* READ, READ | WRITE */
    u32 nb = 2;
    if (x1) { bo[nb].flags = 3; bo[nb].handle = x1->handle; nb++; }
    if (x2) { bo[nb].flags = 3; bo[nb].handle = x2->handle; nb++; }
    struct { u32 type, idx, off, size, pad, nr_relocs; u64 relocs; } c = { 1, 0, off, bytes, 0, 0, 0 };   /* MSM_SUBMIT_CMD_BUF */
    struct { u32 flags, fence, nr_bos, nr_cmds; u64 bos, cmds; int fence_fd; u32 queueid; u64 in_s, out_s; u32 nin, nout, stride, pad; } sb;
    memset(&sb, 0, sizeof sb);
    sb.flags = 0x10 | 0x80000000u;       /* MSM_PIPE_3D0 | MSM_SUBMIT_NO_IMPLICIT */
    sb.nr_bos = nb; sb.nr_cmds = 1;
    sb.bos = (u64)(usize)bo; sb.cmds = (u64)(usize)&c;
    sb.fence_fd = -1;
    long r = S(NR_IOCTL, gfd, MSM_GEM_SUBMIT, &sb, 0, 0);
    *fence = sb.fence;
    return r;
}
/* wait for a fence (timeout 0: only look - -ETIMEDOUT if not yet) */
static long wait_fence(u32 fence, u32 timeout_ms) {
    struct { long long s, ns; } now = { 0, 0 };
    if (timeout_ms) {
        S(113, 1, &now, 0, 0, 0);        /* clock_gettime(CLOCK_MONOTONIC): the wait's end is absolute */
        now.ns += (long long)timeout_ms * 1000000;
        now.s += now.ns / 1000000000; now.ns %= 1000000000;
    }
    struct { u32 fence, flags; long long s, ns; u32 queueid, pad; } w = { fence, 0, now.s, now.ns, 0, 0 };
    return S(NR_IOCTL, gfd, MSM_WAIT_FENCE, &w, 0, 0);
}
/* run what is in cmd (from its start to cp) and wait for it: 0, or why not.  Every
 * buffer the stream touches is listed (the driver maps a buffer's pages for the GPU
 * when a submit lists it): cmd, the scratch page, and up to two more */
static long run(u32 timeout_ms, const struct bo *x1, const struct bo *x2) {
    u32 f;
    long r = submit(0, (u32)((u8 *)cp - (u8 *)cmd.cpu), x1, x2, &f);
    return r < 0 ? r : wait_fence(f, timeout_ms);
}

/* check 1: the command processor runs QRT's packets - a value written to memory */
static int test_write(void) {
    cp = cmd.cpu;
    scr.cpu[0] = 0;
    out(pkt7(CP_MEM_WRITE, 3)); out((u32)scr.iova); out((u32)(scr.iova >> 32)); out(0x51525427);
    u64 t0 = k_now_us();
    long r = run(1000, NULL, NULL);
    u32 got = scr.cpu[0];
    if (r < 0 || got != 0x51525427) { LOG("QRT's first command stream: %ld, read back %x", r, got); return 0; }
    LOG("QRT's commands run: a CP_MEM_WRITE came back in %llu us", k_now_us() - t0);
    return 1;
}
/* check 2: the 2D engine copies a 64 x 64 picture exactly - each way of starting it is
 * tried (and a solid fill, which says whether it writes at all); every result is logged */
static int test_copy(void) {
    struct bo a, b;
    if (!bo_new(&a, 64 << 10) || !bo_new(&b, 64 << 10)) return 0;
    for (u32 i = 0; i < 64 * 64; i++) a.cpu[i] = i * 2654435761u + 0x01020304u;
    /* BLIT_OP_COPY first: on the Mi A1 every COPY worked, no SCALE wrote (0.27.1) */
    static const u8 modes[] = { RM_BLIT2DSCALE, 5 /* BLIT2D */ }, ops[] = { 1, 3 }, lens[] = { 5, 1 };
    static const u32 r84[] = { 9, 1 };
    nbv = 0;
    for (int m = 0; m < 2; m++) for (int o = 0; o < 2; o++) for (int l = 0; l < 2; l++) for (int r = 0; r < 2; r++)
        bvs[nbv++] = (struct bv){ modes[m], ops[o], lens[l], r84[r] };
    char line[160];
    int ll = 0;
    line[0] = 0;
    for (int i = 0; i < nbv + 4; i++) {
        struct bv fill = { i - nbv < 2 ? 5 : RM_BLIT2DSCALE, 0, (i - nbv) & 1 ? 1 : 5, 9 };
        const struct bv *v = i < nbv ? &bvs[i] : &fill;
        for (u32 k = 0; k < 64 * 64; k++) b.cpu[k] = 0;
        cp = cmd.cpu;
        blit(v, a.iova, 256, b.iova, 256, 0, 0, 0, 0, 64, 64, 0x5a5a5a5a);
        long rr = run(3000, &a, &b);
        int good = 0, written = 0;
        for (u32 k = 0; k < 64 * 64; k++) {
            u32 want = v->op ? a.cpu[k] : 0x5a5a5a5au;
            good += b.cpu[k] == want;
            written += b.cpu[k] != 0;
        }
        if (i < nbv && good == 4096 && !use_bv) use_bv = v;
        if (use_bv) break;
        ll += (int)fmt(line + ll, sizeof line - (usize)ll, " %s m%u op%u n%u r%u:%d/%d%s", i < nbv ? "" : "fill", v->mode, v->op,
                       v->rmlen, v->r2184, good, written, rr < 0 ? "!" : "");
        if ((i & 3) == 3) { LOG("2D tries (good/written):%s", line); ll = 0; line[0] = 0; }
    }
    if (ll && (!use_bv || use_bv != &bvs[0])) LOG("2D tries (good/written):%s", line);
    if (!use_bv) { LOG("the 2D engine copied exactly in none of them"); return 0; }
    LOG("the 2D engine copies: mode %u, op %u, %u-dword render mode, 2184=%u", use_bv->mode, use_bv->op, use_bv->rmlen, use_bv->r2184);
    return 1;
}

/* the shell's frames, mapped for the GPU as they are first seen (they live as long as the
 * shell's layout: a rotation makes new ones) */
static struct { u64 pa, len, iova; } maps[12];
static int nmaps;
static u64 frame_iova(const u32 *px, u64 len) {
    u64 pa = (u64)(usize)px;
    for (int i = 0; i < nmaps; i++) if (pa >= maps[i].pa && pa + len <= maps[i].pa + maps[i].len) return maps[i].iova + (pa - maps[i].pa);
    if (nmaps == (int)(sizeof maps / sizeof maps[0]) || !arm_is_ram(pa, len)) return 0;
    u64 a = pa & ~4095ull, l = ((pa + len + 4095) & ~4095ull) - a;
    u64 v = map_phys(a, l);
    if (!v) return 0;
    maps[nmaps].pa = a; maps[nmaps].len = l; maps[nmaps].iova = v; nmaps++;
    return v + (pa - a);
}
/* the CPU drew the frame in cached memory: written back to RAM, where the GPU reads */
static struct { const u32 *px; usize stride; int x, y, w, h; } cj;
static void clean_band(void *arg, int i, int n) {
    (void)arg;
    for (int r = cj.h * i / n; r < cj.h * (i + 1) / n; r++) {
        u64 a = (u64)(usize)(cj.px + (usize)(cj.y + r) * cj.stride + cj.x), e = a + (u64)cj.w * 4;
        for (a &= ~63ull; a < e; a += 64) __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void fb_gpu_presented(int x, int y, int w, int h, u64 us);
int logview_active(void);
int gpu_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h) {
    if (!g_on || logview_active() || rot || sw != (int)k.fb_w || sh != (int)k.fb_h || ((usize)src & 63) || ((u32)stride * 4) & 63) return 0;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return 1;
    /* behind (a fence came late - Wi-Fi's start in the secure world stalled the phone for
     * 0.6 s and 0.27.1 turned the GPU off for good): the CPU draws until the GPU's last
     * copy is done; then the whole frame once, over whatever the late copies wrote */
    if (behind) {
        if (wait_fence(last_fence, 0) < 0) return 0;
        behind = 0;
        x = 0; y = 0; w = sw; h = sh;
    }
    u64 t0 = k_now_us();
    u64 s = frame_iova(src, (u64)stride * 4 * (u64)sh);
    if (!s) return 0;
    cj.px = src; cj.stride = (usize)stride; cj.x = x; cj.y = y; cj.w = w; cj.h = h;
    int bands = (u64)w * (u64)h >= 40000 ? MIN(16, MIN(h, 2 * (smp_workers() + 1))) : 1;
    if (bands > 1) smp_run(clean_band, NULL, bands);
    else clean_band(NULL, 0, 1);
    u64 t1 = k_now_us();
    /* no waiting for the copy (a round trip through Linux took ~28 ms): the stream goes
     * in one of NSLOT slots of the command buffer, whose last use must be done first */
    u32 slot = nsub % NSLOT;
    long r = 0;
    if (slot_fence[slot] && (r = wait_fence(slot_fence[slot], 0)) < 0) {
        u64 tw = k_now_us();
        r = wait_fence(slot_fence[slot], 100);
        st_block += k_now_us() - tw;
        if (r < 0) { behind = 1; st_behind++; return 0; }       /* this frame: the CPU's */
    }
    u32 f = 0;
    if (r >= 0) {
        cp = cmd.cpu + slot * (SLOT_BYTES / 4);
        blit(use_bv, s, (u32)stride * 4, fb_iova, fb_pitch, x, y, x, y, w, h, 0);
        r = submit(slot * SLOT_BYTES, (u32)((u8 *)cp - (u8 *)(cmd.cpu + slot * (SLOT_BYTES / 4))), NULL, NULL, &f);
    }
    if (r < 0) {                         /* the submit failed: the CPU draws from now on */
        g_on = 0;
        fmt(state, sizeof state, "off: a frame copy failed (%ld) - the CPU draws", r);
        LOG("%s", state);
        return 0;
    }
    slot_fence[slot] = f;
    last_fence = f;
    nsub++;
    u64 t2 = k_now_us(), us = t2 - t0;
    st_clean += t1 - t0; st_submit += t2 - t1; st_n++;
    if (t2 - st_t0 >= 10000000) {
        if (st_n) LOG("10 s: %u frames copied; each: cache write-back %llu us, submit %llu us, waited for a slot %llu us%s", st_n,
                      st_clean / st_n, (st_submit - st_block) / st_n, st_block / st_n, st_behind ? "; the GPU fell behind (the CPU drew meanwhile)" : "");
        st_t0 = t2; st_n = 0; st_clean = st_submit = st_block = 0; st_behind = 0;
    }
    g_frames++; g_us += (u32)us;
    fb_gpu_presented(x, y, w, h, us);
    return 1;
}
int gpu_active(void) { return g_on; }
void gpu_stats(u32 *frames, u32 *avg_us, int *coherent) {
    *frames = g_frames; *avg_us = g_frames ? g_us / g_frames : 0; *coherent = 0;
    g_frames = g_us = 0;
}

int fb_wants_32(void);
int fb_switch_32(void);
u32 fb32_pitch(void);
extern u64 fb32_base, fb32_size;
/* after "running": QRT's own commands, then the screen */
static void own_commands(long fd) {
    gfd = fd;
    if (!bo_new(&cmd, 64 << 10) || !bo_new(&scr, 4096)) return;
    plog_state(PLOG_GPU_STARTING);       /* a hang that resets the phone: not again next boot */
    int ok = test_write() && test_copy();
    plog_state(PLOG_LINUX_OK);
    if (!ok) { fmt(state + strlen(state), sizeof state - strlen(state), "; QRT's commands failed (see the log)"); return; }
    int ch = fdt_node("/chosen"), al;
    const char *args = ch >= 0 ? fdt_prop(ch, "bootargs", &al) : NULL;
    /* copying the CPU's frames does not pay (0.27.2 on the Mi A1): the CPU must write its
     * cache back over every changed pixel first - ~12 ms a full frame, the memory traffic
     * of its own copy - and a copy not waited for tears while scrolling.  The GPU earns its
     * keep when it draws; until then its frame copies are there for trying (qrt.gpudraw) */
    if (!(args && strstr(args, "qrt.gpudraw"))) { LOG("ready; the CPU puts the frames on the screen (qrt.gpudraw: the GPU)"); fmt(state + strlen(state), sizeof state - strlen(state), "; ready for QRT's commands"); return; }
    if (!fb32_base || !fb_switch_32()) { LOG("no 32-bit screen: frames stay the CPU's"); return; }
    fb_iova = map_phys(fb32_base, fb32_size);
    if (!fb_iova) return;
    fb_pitch = fb32_pitch();
    __asm__ volatile("dmb ish" ::: "memory");
    g_on = 1;
    fmt(state + strlen(state), sizeof state - strlen(state), "; puts the frames on the screen");
    LOG("the GPU copies the shell's frames to the screen from now on");
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
int  gpu_active(void) { return 0; }
int  gpu_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h) { (void)src; (void)sw; (void)sh; (void)stride; (void)rot; (void)x; (void)y; (void)w; (void)h; return 0; }
void gpu_stats(u32 *frames, u32 *avg_us, int *coherent) { *frames = 0; *avg_us = 0; *coherent = 0; }
#endif
