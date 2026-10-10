/*
 * linux.c - Linux's own drivers inside Tessera on ARM phones (LKL, the Linux kernel as a
 * library, linked into this kernel: ports/lkl-arm64.sh).
 *
 * Tessera passes Linux the boot loader's device tree; Linux populates the SoC's
 * platform devices and its Qualcomm drivers bring the phone up the way they do on
 * Linux: clocks, pins, the RPM co-processor and its regulators, I2C, the touch screen,
 * the power key, the backlight, eMMC.  Tessera provides what LKL asks of a host:
 *
 *   threads, semaphores, mutexes, TLS   sched.c (cooperative, one core)
 *   timers                              a timer thread over a list of one-shot timers
 *   RAM                                 contiguous pages below 4 GB (DMA addresses are
 *                                       physical addresses: the map is 1:1)
 *   device registers                    ioremap is the identity, accesses are volatile
 *   interrupts                          the GIC: Linux's qrt-gic irqchip (LKL patch)
 *                                       masks and configures lines; a poller here takes
 *                                       them from GICC_IAR and runs Linux's handlers
 *   coherent DMA memory                 an uncached pool (mmu.c maps it Normal-NC)
 *
 * What Linux's drivers make comes back into Tessera: evdev devices (touch screen, keys)
 * as shell events (linux_input_poll, from hal_poll).
 */
#include "arm.h"
#include "sched.h"

#ifdef QRT_LKL
#include <lkl/asm/host_ops.h>

/* lkl.o */
int  lkl_init(struct lkl_host_operations *ops);
int  lkl_start_kernel(const char *cmd, ...);
int  lkl_trigger_irq(int irq);
long lkl_syscall(long no, long *params);
extern void *lkl_qrt_fdt;
extern unsigned long lkl_qrt_dma_base, lkl_qrt_dma_size;
extern unsigned long qrt_gic_cpu_base;

/* the few system calls used here (asm-generic numbers, as LKL uses) */
#define NR_MKDIRAT    34
#define NR_MOUNT      40
#define NR_OPENAT     56
#define NR_CLOSE      57
#define NR_GETDENTS64 61
#define NR_READ       63
#define NR_IOCTL      29
static long sys(long nr, long a, long b, long c, long d, long e) { long p[6] = { a, b, c, d, e, 0 }; return lkl_syscall(nr, p); }
#define AT_FDCWD (-100)
static long l_open(const char *p, int fl) { return sys(NR_OPENAT, AT_FDCWD, (long)p, fl, 0, 0); }
static long l_read(int fd, void *b, long n) { return sys(NR_READ, fd, (long)b, n, 0, 0); }
static long l_close(int fd) { return sys(NR_CLOSE, fd, 0, 0, 0, 0); }
static long l_ioctl(int fd, unsigned long cmd, void *arg) { return sys(NR_IOCTL, fd, (long)cmd, (long)arg, 0, 0); }

/* what LKL's own host library would provide */
int lkl_printf(const char *f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    int n = vfmt(b, sizeof b, f, ap);
    va_end(ap);
    klog("linux: %s", b);
    return n;
}
void lkl_bug(const char *f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    vfmt(b, sizeof b, f, ap);
    va_end(ap);
    native_panic(b, NULL);
}
unsigned long __getauxval(unsigned long type) { (void)type; return 0; }   /* libgcc's atomics: no LSE */

/* Linux's idle loop (LKL patch): let the timer and interrupt threads run */
void lkl_qrt_idle(void) { thr_yield(); }

static int running, dead;
int linux_running(void) { return running && !dead; }

/* Argon's interrupts.  LKL takes an interrupt only when the thread holding Linux's CPU
 * lets it (cpu_relax, local_irq_restore, giving the CPU up); a driver that spins on
 * jiffies, or on a flag its interrupt sets, without doing so waits forever - the timer
 * tick and the touch screen's interrupt sit pending behind it.  A real CPU takes the
 * interrupt on top of whatever runs; so does this: Tessera's tick, returning into
 * Linux's own code (not into a host operation it called), runs what Linux has pending
 * when Linux's interrupts are on.  IRQs stay unmasked meanwhile (the frame and the
 * FP/SIMD registers are saved on entry), so the shell keeps its tick. */
int lkl_qrt_irq_tail(void);
extern char __lkl_text_start[], __lkl_text_end[];
static u64 tail_runs, tail_why[8];
static thr_t *timer_thr;
static int lkl_up;                                   /* from lkl_start_kernel() on */
void argon_irq_tail(u64 pc) {
    if (!lkl_up || dead || pc < (u64)(usize)__lkl_text_start || pc >= (u64)(usize)__lkl_text_end) return;
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    int r = lkl_qrt_irq_tail();
    if (r > 0) tail_runs++;
    else tail_why[-r & 7]++;
    __asm__ volatile("msr daifset, #2" ::: "memory");
}
void linux_failed(const char *why) { dead = 1; klog("linux: stopped (%s); the shell goes on - this log is in System", why); }

/* ---- the host operations ---- */
static char pline[256];
static int plen;
/* Linux's complaints, kept for the summary (linux_summary): on a phone screen the log's
 * tail is all that shows, and the reasons scroll away */
#define NPROB 32
static char probs[NPROB][112];
static int nprob;
static int read_text(const char *path, char *out, int cap);
/* Linux's lines about the Wi-Fi core, kept for lwifi.c to show when it does not start */
#define NWIFI 32                                        /* the last 32: a ring */
static char wlines[NWIFI][120];
static int nwl;
static void keep_wifi(const char *l) {
    static const char *const w[] = { "wcnss", "pronto", "scm", "PAS", "iris", "remoteproc", "a204000", "wcn36xx", "smsm", "smp2p",
                                     "wlan", "firmware", "nv.bin", "ieee80211", "phy0", "rfkill", NULL };
    if (strstr(l, "initcall") || strstr(l, "calling ")) return;
    for (int i = 0; w[i]; i++)
        if (strstr(l, w[i])) {
            const char *m = l[0] == '[' && strchr(l, ']') ? strchr(l, ']') + 2 : l;
            strlcpy(wlines[nwl++ % NWIFI], m, sizeof wlines[0]);
            return;
        }
}
/* the same for sound: the audio DSP, APR and its services, the card, the codecs */
#define NSND 40
static char slines[NSND][120];
static int nsl;
static void keep_sound(const char *l) {
    static const char *const w[] = { "adsp", "c200000", "lpass", "apr", "q6", "snd", "sound", "asoc", "ASoC", "wcd", "max98927",
                                     "MI2S", "amplifier", "audio", "dai", NULL };
    if (strstr(l, "initcall") || strstr(l, "calling ")) return;
    for (int i = 0; w[i]; i++)
        if (strstr(l, w[i])) {
            const char *m = l[0] == '[' && strchr(l, ']') ? strchr(l, ']') + 2 : l;
            strlcpy(slines[nsl++ % NSND], m, sizeof slines[0]);
            return;
        }
}
static void list_dir(const char *what, const char *dir) {
    char line[200] = "";
    long fd = l_open(dir, 0200000);
    if (fd >= 0) {
        static char d[2048];
        long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
        l_close((int)fd);
        for (long o = 0; o < n; o += *(u16 *)(d + o + 16)) {
            const char *nm = d + o + 19;
            if (nm[0] == '.') continue;
            usize l = strlen(line);
            fmt(line + l, sizeof line - l, " %s", nm);
        }
    }
    klog("sound: %s:%s", what, line[0] ? line : fd < 0 ? " (none - no such bus)" : " none");
}
/* the audio devices (codecs, the card, the amplifier): which have a driver, and asking
 * Linux to try again for those without one - a probe that failed while the DSP was not
 * up yet (its clocks come through it) is not retried by Linux by itself */
#define NR_SYSLOG 116
static void write_text(const char *path, const char *text);
static int sound_dev(const char *nm) {
    return strstr(nm, "codec") || strstr(nm, "sound") || strstr(nm, "amplifier") || strstr(nm, "max98927") || strstr(nm, "-003a");
}
int argon_sound_reprobe(int report) {
    static const char *const dirs[] = { "/sys/bus/platform/devices", "/sys/bus/i2c/devices", NULL };
    int unbound = 0;
    for (int b = 0; dirs[b]; b++) {
        long fd = l_open(dirs[b], 0200000);
        if (fd < 0) continue;
        static char d[8192];
        long n;
        while ((n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0)) > 0)
            for (long o = 0; o < n; o += *(u16 *)(d + o + 16)) {
                const char *nm = d + o + 19;
                if (nm[0] == '.' || !sound_dev(nm)) continue;
                char p[160];
                fmt(p, sizeof p, "%s/%s/driver", dirs[b], nm);
                long dfd = l_open(p, 0200000);
                int bound = dfd >= 0;
                if (dfd >= 0) l_close((int)dfd);
                if (report) klog("sound: device %s: %s", nm, bound ? "has its driver" : "NO driver");
                if (!bound) {
                    unbound++;
                    write_text(b == 0 ? "/sys/bus/platform/drivers_probe" : "/sys/bus/i2c/drivers_probe", nm);
                }
            }
        l_close((int)fd);
    }
    return unbound;
}
void argon_sound_report(void) {
    int first = nsl > NSND ? nsl - NSND : 0;
    for (int i = first; i < nsl; i++) klog("sound: linux said: %s", slines[i % NSND]);
    if (!nsl) {                                                     /* not on the console: Linux's own log */
        static char kb[65536];
        long n = sys(NR_SYSLOG, 3, (long)kb, sizeof kb - 1, 0, 0);   /* SYSLOG_ACTION_READ_ALL */
        kb[n > 0 ? n : 0] = 0;
        static const char *lines[40];
        int nl = 0;
        for (char *l = kb; *l; ) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            int hit = 0;
            if (!strstr(l, "initcall") && !strstr(l, "calling ")) {
                static const char *const w[] = { "adsp", "c200000", "apr", "q6", "snd", "sound", "asoc", "ASoC", "wcd", "codec", "max98927",
                                                 "MI2S", "amplifier", "dai", NULL };
                for (int i = 0; w[i] && !hit; i++) hit = strstr(l, w[i]) != NULL;
            }
            if (hit) lines[nl++ % 40] = l;
            if (!e) break;
            l = e + 1;
        }
        for (int i = nl > 40 ? nl - 40 : 0; i < nl; i++) {
            const char *m = lines[i % 40];
            if (*m == '<' && strchr(m, '>')) m = strchr(m, '>') + 1;
            if (*m == '[' && strchr(m, ']')) m = strchr(m, ']') + 2;
            klog("sound: linux said: %s", m);
        }
        if (!nl) klog("sound: Linux's log has nothing about the audio DSP or the card (%ld bytes read)", n);
    }
    argon_sound_reprobe(1);
    list_dir("SMD channels (rpmsg)", "/sys/bus/rpmsg/devices");
    list_dir("DSP services (APR)", "/sys/bus/aprbus/devices");
    list_dir("sound devices", "/sys/class/sound");
    long fd = l_open("/sys/kernel/debug/devices_deferred", 0);       /* who waits, and for what */
    if (fd >= 0) {
        static char buf[4096];
        long len = l_read((int)fd, buf, sizeof buf - 1);
        l_close((int)fd);
        buf[len > 0 ? len : 0] = 0;
        int n = 0;
        for (char *l = buf; *l; ) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            for (char *t = l; *t; t++) if (*t == '\t') *t = ' ';
            if (*l) { klog("sound: waiting: %s", l); n++; }
            if (!e) break;
            l = e + 1;
        }
        if (!n) klog("sound: no device waiting");
    }
}
void argon_wifi_report(void) {
    int first = nwl > NWIFI ? nwl - NWIFI : 0;
    for (int i = first; i < nwl; i++) klog("wifi: linux said: %s", wlines[i % NWIFI]);
    if (!nwl) klog("wifi: Linux said nothing about the Wi-Fi core");
    static const char *const devs[][2] = { { "a204000.remoteproc", "the Pronto core" }, { "a204000.remoteproc:iris", "its RF chip (iris)" },
                                           { "firmware:scm", "the secure world (SCM)" } };
    for (usize i = 0; i < ARRAY_LEN(devs); i++) {
        char p[128], t[8];
        fmt(p, sizeof p, "/sys/bus/platform/devices/%s/uevent", devs[i][0]);
        int there = read_text(p, (char[256]){ 0 }, 256) > 0;
        fmt(p, sizeof p, "/sys/bus/platform/devices/%s/driver/uevent", devs[i][0]);
        long fd = l_open(p, 0);
        int bound = fd >= 0;
        if (fd >= 0) l_close((int)fd);
        (void)t;
        klog("wifi: %s (%s): %s", devs[i][1], devs[i][0], !there ? "no such device" : bound ? "bound to its driver" : "NOT bound");
    }
    long fd = l_open("/sys/class/remoteproc", 0200000);
    if (fd < 0) { klog("wifi: no remoteproc class in Linux"); return; }
    static char d[2048];
    long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
    l_close((int)fd);
    int any = 0;
    for (long o = 0; o < n; o += *(u16 *)(d + o + 16)) {
        const char *nm = d + o + 19;
        if (strncmp(nm, "remoteproc", 10)) continue;
        char sp[96], f[64] = "", st[32] = "";
        fmt(sp, sizeof sp, "/sys/class/remoteproc/%s/firmware", nm); read_text(sp, f, sizeof f);
        fmt(sp, sizeof sp, "/sys/class/remoteproc/%s/state", nm); read_text(sp, st, sizeof st);
        for (char *c = f; *c; c++) if (*c == '\n') *c = 0;
        for (char *c = st; *c; c++) if (*c == '\n') *c = 0;
        klog("wifi: %s: firmware %s, %s", nm, f, st);
        any = 1;
    }
    if (!any) klog("wifi: Linux has no remote processors registered");
}
static void keep_problem(const char *l) {
    keep_wifi(l);
    keep_sound(l);
    static const char *const words[] = { "error", "fail", "Fail", "unable", "Unable", "invalid", "not found", "No ", "timed out", "timeout",
                                         "smd:", "smem:", "rpm:", "probe of remoteproc", NULL };
    if (nprob == NPROB || strstr(l, "initcall") || strstr(l, "calling ") || strstr(l, "initial console")) return;
    int hit = 0;
    if (strstr(l, "probe of ") && (strstr(l, "rpm") || strstr(l, "regulator") || strstr(l, "controller") || strstr(l, "i2c") ||
                                   strstr(l, "mmc") || strstr(l, "1-00")))
        hit = 1;                                                  /* what the touch screen and storage hang on */
    for (int i = 0; words[i] && !hit; i++) hit = strstr(l, words[i]) != NULL;
    const char *ret = strstr(l, " returned -");                  /* initcall_debug: a probe that gave up quietly */
    if (ret && strstr(l, "probe of ") && strncmp(ret, " returned -517", 14)) hit = 1;
    if (strstr(l, "ignoring dependency")) hit = 1;
    if (!hit) return;
    const char *m = l[0] == '[' && strchr(l, ']') ? strchr(l, ']') + 2 : l;  /* without the timestamp */
    strlcpy(probs[nprob++], m, sizeof probs[0]);
}
/* the fuel gauge's own lines, for battery_why */
static char fgl[3][120];
static int nfg;
static void keep_fg(const char *l) {
    if (!strstr(l, "fuel-gauge") && !strstr(l, "qcom-fg") && !strstr(l, "qcom_fg") && !strstr(l, "qcom-battery")) return;
    const char *m = l[0] == '[' && strchr(l, ']') ? strchr(l, ']') + 2 : l;
    strlcpy(fgl[nfg % 3], m, sizeof fgl[0]);
    nfg++;
}
static void h_print(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n' || plen == (int)sizeof pline - 1) {
            pline[plen] = 0;
            if (plen) { klog("linux: %s", pline); keep_problem(pline); keep_fg(pline); }
            plen = 0;
        }
        if (s[i] != '\n') pline[plen++] = s[i];
    }
}
static void h_panic(void) { linux_failed("Linux panicked"); thr_park(); }
static struct lkl_sem *h_sem_alloc(int c) { return (struct lkl_sem *)sem_new(c); }
static void h_sem_free(struct lkl_sem *s) { sem_del((sem_t *)s); }
static void h_sem_up(struct lkl_sem *s) { sem_up((sem_t *)s); }
static void h_sem_down(struct lkl_sem *s) { sem_down((sem_t *)s); }
static struct lkl_mutex *h_mutex_alloc(int r) { return (struct lkl_mutex *)mtx_new(r); }
static void h_mutex_free(struct lkl_mutex *m) { mtx_del((mtx_t *)m); }
static void h_mutex_lock(struct lkl_mutex *m) { mtx_lock((mtx_t *)m); }
static void h_mutex_unlock(struct lkl_mutex *m) { mtx_unlock((mtx_t *)m); }
static lkl_thread_t h_thread_create(void (*f)(void *), void *arg) { return (lkl_thread_t)(usize)thr_create("linux", f, arg, 96 << 10); }
static void h_thread_detach(void) { thr_detach(thr_self()); }
static void h_thread_exit(void) { thr_exit(); }
static int h_thread_join(lkl_thread_t t) { return thr_join((thr_t *)(usize)t); }
static lkl_thread_t h_thread_self(void) { return (lkl_thread_t)(usize)thr_self(); }
static int h_thread_equal(lkl_thread_t a, lkl_thread_t b) { return a == b; }
static struct lkl_tls_key *h_tls_alloc(void (*d)(void *)) { (void)d; int k = tls_key_new(); return k < 0 ? NULL : (struct lkl_tls_key *)(usize)(k + 1); }
static void h_tls_free(struct lkl_tls_key *k) { tls_key_del((int)(usize)k - 1); }
static int h_tls_set(struct lkl_tls_key *k, void *v) { tls_put((int)(usize)k - 1, v); return 0; }
static void *h_tls_get(struct lkl_tls_key *k) { return tls_fetch((int)(usize)k - 1); }
static void *h_mem_alloc(unsigned long n) { return kalloc(n); }
static void h_mem_free(void *p) { kfree(p); }
static void *h_page_alloc(unsigned long n) { return (void *)(usize)pmm_alloc_contig((n + 4095) / 4096); }
static void h_page_free(void *p, unsigned long n) { (void)p; (void)n; }
u64 k_now_ns(void);
static unsigned long long h_time(void) { return k_now_ns(); }
static void *h_ioremap(long addr, int size) { (void)size; return (void *)(usize)addr; }

/* ---- Argon: Linux's virtual memory (LKL in MMU mode) ----
 * Linux's RAM is one physically contiguous block; Linux maps pages of it (and nothing
 * else) into Argon's window (mmu.c) through these.  Its DMA addresses are translated
 * back to the block's real addresses with lkl_qrt_ram_phys (qrt_soc.c). */
int argon_map(u64 va, u64 pa, u64 size);
int argon_unmap(u64 va, u64 size);
extern unsigned long lkl_qrt_ram_phys __attribute__((weak));
static u64 shm_phys, shm_size;
static void h_shmem_init(unsigned long size) {
    shm_size = (size + 4095) & ~4095ul;
    shm_phys = pmm_alloc_contig(shm_size / 4096);
    if (!shm_phys) panic("argon: no contiguous block for Linux's memory");
    if (&lkl_qrt_ram_phys) lkl_qrt_ram_phys = shm_phys;
    klog("argon: Linux's memory: %llu MB at %llx", (u64)shm_size >> 20, shm_phys);
}
static void *h_shmem_mmap(void *addr, unsigned long off, unsigned long size, enum lkl_prot prot) {
    (void)prot;
    if (!shm_phys || off + size > shm_size || argon_map((u64)(usize)addr, shm_phys + off, size) < 0) return (void *)-1;
    return addr;
}
static int h_munmap(void *addr, unsigned long size) { return argon_unmap((u64)(usize)addr, size); }
static int h_iomem(const volatile void *a, void *v, int size, int write) {
    switch (size) {
    case 1: if (write) *(volatile u8 *)a = *(u8 *)v; else *(u8 *)v = *(volatile u8 *)a; break;
    case 2: if (write) *(volatile u16 *)a = *(u16 *)v; else *(u16 *)v = *(volatile u16 *)a; break;
    case 4: if (write) *(volatile u32 *)a = *(u32 *)v; else *(u32 *)v = *(volatile u32 *)a; break;
    case 8: if (write) *(volatile u64 *)a = *(u64 *)v; else *(u64 *)v = *(volatile u64 *)a; break;
    default: return -1;
    }
    return 0;
}
int  arm_setjmp(unsigned long *buf) __attribute__((returns_twice));
void arm_longjmp(unsigned long *buf, int v) __attribute__((noreturn));
/* f must not be a tail call: a longjmp comes back into this frame */
__attribute__((noinline)) static void h_jmp_set(struct lkl_jmp_buf *j, void (*f)(void)) {
    if (!arm_setjmp(j->buf)) f();
    __asm__ volatile("" ::: "memory");
}
static void h_jmp_long(struct lkl_jmp_buf *j, int v) { arm_longjmp(j->buf, v); }
static void *h_memcpy(void *d, const void *s, unsigned long n) { return memcpy(d, s, n); }
static void *h_memset(void *d, int c, unsigned long n) { return memset(d, c, n); }
static void *h_memmove(void *d, const void *s, unsigned long n) { return memmove(d, s, n); }

/* one-shot timers: a list the timer thread runs */
struct qtimer { void (*fn)(void); u64 due; int armed; struct qtimer *next; };
static struct qtimer *timers;
static void *h_timer_alloc(void (*fn)(void)) {
    struct qtimer *t = kalloc(sizeof *t);
    t->fn = fn;
    u64 f = irq_save();
    t->next = timers;
    timers = t;
    irq_restore(f);
    return t;
}
static int h_timer_set(void *p, unsigned long ns) {
    struct qtimer *t = p;
    u64 f = irq_save();
    t->due = k_now_us() + ns / 1000;
    t->armed = 1;
    irq_restore(f);
    thr_wake(timer_thr);
    return 0;
}
static void h_timer_free(void *p) { ((struct qtimer *)p)->armed = 0; }
static void timer_loop(void *a) {
    (void)a;
    for (;;) {
        u64 now = k_now_us(), next = now + 10000;
        for (struct qtimer *t = timers; t; t = t->next) {
            u64 f = irq_save();
            int fire = t->armed && t->due <= now;
            if (fire) t->armed = 0;
            else if (t->armed && t->due < next) next = t->due;
            irq_restore(f);
            if (fire) { t->fn(); now = k_now_us(); }
        }
        thr_sleep_until(next);
    }
}

static struct lkl_host_operations ops = {
    .print = h_print, .panic = h_panic,
    .sem_alloc = h_sem_alloc, .sem_free = h_sem_free, .sem_up = h_sem_up, .sem_down = h_sem_down,
    .mutex_alloc = h_mutex_alloc, .mutex_free = h_mutex_free, .mutex_lock = h_mutex_lock, .mutex_unlock = h_mutex_unlock,
    .thread_create = h_thread_create, .thread_detach = h_thread_detach, .thread_exit = h_thread_exit,
    .thread_join = h_thread_join, .thread_self = h_thread_self, .thread_equal = h_thread_equal,
    .tls_alloc = h_tls_alloc, .tls_free = h_tls_free, .tls_set = h_tls_set, .tls_get = h_tls_get,
    .mem_alloc = h_mem_alloc, .mem_free = h_mem_free, .page_alloc = h_page_alloc, .page_free = h_page_free,
    .time = h_time, .timer_alloc = h_timer_alloc, .timer_set_oneshot = h_timer_set, .timer_free = h_timer_free,
    .ioremap = h_ioremap, .iomem_access = h_iomem,
    .shmem_init = h_shmem_init, .shmem_mmap = h_shmem_mmap, .munmap = h_munmap,
    .jmp_buf_set = h_jmp_set, .jmp_buf_longjmp = h_jmp_long,
    .memcpy = h_memcpy, .memset = h_memset, .memmove = h_memmove,
};

/* ---- interrupts: Linux's SPIs, pending and enabled in the distributor (gic.c) ---- */
static u64 nirq;
int gic_pending(void (*fn)(u32 id));
static u32 irq_count[1024];                  /* deliveries per interrupt ID, for the summary */
static void deliver(u32 id) { lkl_trigger_irq(1024 + (int)id); nirq++; if (id < 1024) irq_count[id]++; }
static void irq_loop(void *a) {
    (void)a;
    for (;;) {
        gic_pending(deliver);
        thr_sleep_us(1000);
    }
}

/* ---- input: Linux's evdev devices -> the shell's events ---- */
static event_t inq[64];
static int inq_n;
static void inq_put(event_t e) {
    u64 f = irq_save();
    if (e.type == EV_MOVE && inq_n && inq[inq_n - 1].type == EV_MOVE) inq[inq_n - 1] = e;
    else if (inq_n < (int)ARRAY_LEN(inq)) inq[inq_n++] = e;
    irq_restore(f);
}
int linux_input_poll(event_t *out, int max) {
    u64 f = irq_save();
    int n = MIN(inq_n, max);
    memcpy(out, inq, (usize)n * sizeof *out);
    memmove(inq, inq + n, (usize)(inq_n - n) * sizeof *inq);
    inq_n -= n;
    irq_restore(f);
    return n;
}

void arm_key_of(int code, int shift, int ctrl, u16 *scan, c16 *ch);   /* virtio.c */
static volatile int keys_live;          /* Linux has delivered a key: the native volume-up poll stops */
struct ievent { long sec, usec; u16 type, code; i32 value; };
struct indev { int open, fd, abs, min[2], max[2]; char name[16]; };
#define EVIOCGBIT(ev, len) ((2u << 30) | ((u32)(len) << 16) | ('E' << 8) | (0x20 + (ev)))
#define EVIOCGABS(abs)     ((2u << 30) | (24u << 16) | ('E' << 8) | (0x40 + (abs)))
#define EVIOCGNAME(len)    ((2u << 30) | ((u32)(len) << 16) | ('E' << 8) | 0x06)

static void input_loop(void *arg) {
    struct indev *d = arg;
    struct ievent ev[16];
    int shift = 0, ctrl = 0, x = 0, y = 0, touch = 0, was = 0, moved = 0, fingers = 0;
    for (;;) {
        long n = l_read(d->fd, ev, sizeof ev);
        if (n <= 0) {
            if (n == -19) break;                                     /* -ENODEV: unplugged */
            static int said;
            if (n < 0 && said++ < 3) klog("linux: reading %s: error %ld", d->name, n);
            thr_sleep_us(10000);
            continue;
        }
        for (long i = 0; i < n / (long)sizeof ev[0]; i++) {
            struct ievent *e = &ev[i];
            if (e->type == 1) {                                          /* EV_KEY */
                if (e->code == 42 || e->code == 54) shift = e->value != 0;
                else if (e->code == 29 || e->code == 97) ctrl = e->value != 0;
                else if (e->code == 0x14a || e->code == 0x110) touch = e->value != 0;   /* BTN_TOUCH, BTN_LEFT */
                else if (e->value) {
                    event_t k = { .type = EV_KEY };
                    arm_key_of(e->code, shift, ctrl, &k.scan, &k.ch);
                    if (k.scan || k.ch) { keys_live = 1; inq_put(k); }
                }
            } else if (e->type == 3 && d->abs) {                        /* EV_ABS */
                static int said_abs;
                if (said_abs++ == 0) klog("linux: first touch data from %s (axis %u = %d)", d->name, e->code, e->value);
                int axis = e->code == 0 ? 0 : e->code == 1 ? 1 : -1;
                if (e->code == 0x39) fingers += e->value >= 0 ? 1 : -1;  /* ABS_MT_TRACKING_ID */
                if (axis < 0) continue;
                int span = d->max[axis] - d->min[axis];
                int px = span > 0 ? (int)((i64)(e->value - d->min[axis]) * ((axis ? (int)k.fb_h : (int)k.fb_w) - 1) / span) : 0;
                if (px < 0) px = 0;                                      /* a controller reporting past the panel */
                if (axis && px >= (int)k.fb_h) px = (int)k.fb_h - 1;
                if (!axis && px >= (int)k.fb_w) px = (int)k.fb_w - 1;
                if (axis) y = px; else x = px;
                moved = 1;
            } else if (e->type == 0 && e->code == 0 && d->abs) {         /* SYN_REPORT */
                int f = fingers > 0 ? fingers : touch;
                if (f > 5) f = 5;                                        /* a count gone astray */
                if (touch && !was) {
                    event_t t = { .type = EV_DOWN, .x = x, .y = y, .fingers = f };
                    inq_put(t);
                    static int said;                             /* the first touches, for the log */
                    if (said++ < 5) klog("linux: touch down at %d,%d (%s)", x, y, d->name);
                }
                else if (touch && moved) { event_t t = { .type = EV_MOVE, .x = x, .y = y, .fingers = f }; inq_put(t); }
                else if (!touch && was) { event_t t = { .type = EV_UP, .x = x, .y = y, .fingers = 0 }; inq_put(t); }
                was = touch; moved = 0;
                if (fingers < 0 || fingers > 10) fingers = touch;
            }
        }
    }
    klog("linux: %s went away", d->name);
    l_close(d->fd);
    d->open = 0;
}

static struct indev indevs[8];
static int have_keys, have_touch;
int input_has_touch(void) { return have_touch || !running; }    /* shell.c: no auto-sleep without one */
int linux_has_keys(void) { return keys_live; }
static int bit(const u8 *b, int n) { return b[n / 8] >> (n % 8) & 1; }
static void input_open(const char *name) {
    for (int i = 0; i < 8; i++) if (indevs[i].open && !strcmp(indevs[i].name, name)) return;   /* fd 0 is valid in Linux */
    struct indev *d = NULL;
    for (int i = 0; i < 8 && !d; i++) if (!indevs[i].open) d = &indevs[i];
    if (!d) return;
    char p[48];
    fmt(p, sizeof p, "/dev/input/%s", name);
    long fd = l_open(p, 0);
    if (fd < 0) return;
    u8 evb[4] = { 0 }, absb[8] = { 0 };
    l_ioctl((int)fd, EVIOCGBIT(0, sizeof evb), evb);
    l_ioctl((int)fd, EVIOCGBIT(3, sizeof absb), absb);
    memset(d, 0, sizeof *d);
    d->open = 1;
    d->fd = (int)fd;
    d->abs = bit(evb, 3) && bit(absb, 0) && bit(absb, 1);
    strlcpy(d->name, name, sizeof d->name);
    for (int a = 0; a < 2 && d->abs; a++) {
        i32 info[6] = { 0 };
        l_ioctl((int)fd, EVIOCGABS(a), info);
        d->min[a] = info[1]; d->max[a] = info[2];
    }
    char nm[64] = "";
    l_ioctl((int)fd, EVIOCGNAME(sizeof nm - 1), nm);
    klog("linux: %s (%s) into Tessera's input%s", name, nm, d->abs ? " (touch)" : "");
    if (strstr(nm, "gpio-keys") || strstr(nm, "gpio_keys")) have_keys = 1;
    if (d->abs) have_touch = 1;
    thr_create("evdev", input_loop, d, 32 << 10);
}
static void input_scan(void *a) {
    (void)a;
    for (;;) {
        long fd = l_open("/dev/input", 0200000);                   /* O_DIRECTORY */
        if (fd >= 0) {
            static char buf[2048];
            long n;
            while ((n = sys(NR_GETDENTS64, fd, (long)buf, sizeof buf, 0, 0)) > 0)
                for (long o = 0; o < n; ) {
                    u16 reclen = *(u16 *)(buf + o + 16);
                    const char *nm = buf + o + 19;
                    if (!strncmp(nm, "event", 5)) input_open(nm);
                    o += reclen;
                }
            l_close((int)fd);
        }
        thr_sleep_us(1000000);
    }
}

/* ---- the backlight: /sys/class/backlight/<first>/ (the phone's WLED) ---- */
static char bl_dir[96];
static int bl_max;
static long read_num(const char *path) {
    char b[24] = "";
    long fd = l_open(path, 0), v = -1;
    if (fd < 0) return -1;
    long n = l_read((int)fd, b, sizeof b - 1);
    l_close((int)fd);
    if (n > 0) { v = 0; for (int i = 0; b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0'); }
    return v;
}
static void bl_find(void) {
    if (bl_dir[0] || !linux_running()) return;
    long fd = l_open("/sys/class/backlight", 0200000);
    if (fd < 0) return;
    static char buf[1024];
    long n = sys(NR_GETDENTS64, fd, (long)buf, sizeof buf, 0, 0);
    for (long o = 0; o < n; ) {
        const char *nm = buf + o + 19;
        if (nm[0] != '.') {
            char p[128];
            fmt(p, sizeof p, "/sys/class/backlight/%s/max_brightness", nm);
            long m = read_num(p);
            if (m > 0) { fmt(bl_dir, sizeof bl_dir, "/sys/class/backlight/%s", nm); bl_max = (int)m; klog("linux: backlight %s (0..%d)", nm, bl_max); break; }
        }
        o += *(u16 *)(buf + o + 16);
    }
    l_close((int)fd);
}
static void bl_write(int pct) {
    char p[128], v[16];
    fmt(p, sizeof p, "%s/brightness", bl_dir);
    int n = fmt(v, sizeof v, "%d", pct <= 0 ? 0 : MAX(1, bl_max * pct / 100));
    long fd = sys(NR_OPENAT, AT_FDCWD, (long)p, 1, 0, 0);             /* O_WRONLY */
    if (fd < 0) return;
    sys(64, fd, (long)v, n, 0, 0);                                     /* write */
    l_close((int)fd);
}
/* the shell never calls into Linux (it would wait while Linux is busy): it leaves the
 * level here and this thread applies it */
static volatile int bl_want = -1, bl_present;
static void bl_loop(void *a) {
    (void)a;
    for (;;) {
        if (linux_running()) {
            bl_find();
            bl_present = bl_dir[0] != 0;
            int w = bl_want;
            if (w >= 0 && bl_present) { bl_want = -1; bl_write(w); }
        }
        thr_sleep_us(100000);
    }
}
int linux_backlight_set(int pct) { bl_want = pct; return 0; }      /* 0..100, applied by bl_loop */
int linux_backlight_present(void) { return bl_present; }

/* ---- Argon display: Linux's display driver (fbdev emulation on its DRM driver) shows
 * Tessera's frames.  Tessera draws into a shadow (fb.c) and this thread writes what
 * changed to /dev/fb0 - a write() marks the framebuffer damaged, so every DRM driver
 * (virtio-gpu, MSM) flushes it to the panel.  The shell never waits on Linux. ---- */
#define NR_PWRITE64 68
int fb_redirect(u8 *shadow, u32 w, u32 h, u32 stride_px, int r, int g, int b);
int fb_take_dirty(int *x, int *y, int *w, int *h);
void shell_redraw(void);
static void display_loop(void *a) {
    (void)a;
    if (fdt_find_compatible(-1, "qrt-,mdss") >= 0) return;          /* the display stays Tessera's (main.c) */
    long fd = -1;
    for (int tries = 0; fd < 0; tries++) {                           /* Linux's display driver probes */
        thr_sleep_us(500000);
        if (dead) return;
        fd = l_open("/dev/fb0", 2);                                   /* O_RDWR */
        if (tries == 120 && fd < 0) klog("argon: no Linux display (/dev/fb0) after 60 s; still waiting - the boot loader's stays meanwhile");
    }
    u8 var[160] = { 0 }, fix[80] = { 0 };
    l_ioctl((int)fd, 0x4600, var);                                    /* FBIOGET_VSCREENINFO */
    l_ioctl((int)fd, 0x4602, fix);                                    /* FBIOGET_FSCREENINFO */
    u32 xres = *(u32 *)(var + 0), yres = *(u32 *)(var + 4), bits = *(u32 *)(var + 24);
    u32 roff = *(u32 *)(var + 32), goff = *(u32 *)(var + 44), boff = *(u32 *)(var + 56);
    u32 line = *(u32 *)(fix + 48);
    klog("argon: Linux's display: %ux%u, %u bits (R at %u, G at %u, B at %u), %u bytes a line", xres, yres, bits, roff, goff, boff, line);
    if (bits != 32 || line < xres * 4) { klog("argon: that pixel format is not one Tessera draws; the boot loader's display stays"); return; }
    u8 *shadow = kalloc((usize)line * yres);
    if (fb_redirect(shadow, xres, yres, line / 4, (int)(roff / 8), (int)(goff / 8), (int)(boff / 8)) < 0) {
        klog("argon: Linux's display is %ux%u, the shell %ux%u: the boot loader's display stays", xres, yres, k.fb_w, k.fb_h);
        return;
    }
    l_ioctl((int)fd, 0x4601, var);                                    /* FBIOPUT_VSCREENINFO: the mode, set */
    l_ioctl((int)fd, 0x4611, 0);                                      /* FBIOBLANK: unblank */
    klog("argon: the screen is Linux's display driver's now");
    shell_redraw();
    for (;;) {
        int x, y, w, h;
        if (fb_take_dirty(&x, &y, &w, &h)) {
            if (x == 0 && w == (int)xres)                             /* whole rows: one write */
                sys(NR_PWRITE64, fd, (long)(shadow + (usize)y * line), (long)h * line, (long)y * line, 0);
            else
                for (int r = y; r < y + h; r++)
                    sys(NR_PWRITE64, fd, (long)(shadow + (usize)r * line + (usize)x * 4), (long)w * 4, (long)r * line + x * 4, 0);
        }
        thr_sleep_us(10000);
    }
}

/* ---- Argon storage: every file system on a disk Linux drives (the eMMC, an SD card)
 * appears in Tessera's file tree as /mnt/<partition>, read-only - nothing on the
 * phone's own partitions can be damaged.  The modem partition is also Linux's
 * firmware: Wi-Fi, modem, DSP and video firmware come from its image/ directory, as
 * postmarketOS's msm-firmware-loader does it. ---- */
#include "../../kernel/vfs.h"
#define NR_NEWFSTATAT 79
#define NR_PREAD64    67
#define NAMNT 24
static struct amnt { char name[40], lpath[56]; } amnts[NAMNT];
static int namnt;
static void join(char *out, usize cap, const struct amnt *m, const char *path) {
    while (*path == '/') path++;
    fmt(out, cap, "%s%s%s", m->lpath, *path ? "/" : "", path);
}
static i64 argon_vfs(void *mnt, int op, const char *path, const char *path2, u64 off, void *buf, u64 len) {
    (void)path2;
    struct amnt *m = mnt;
    char p[512];
    join(p, sizeof p, m, path);
    if (op == VR_READ) {
        long fd = l_open(p, 0);
        if (fd < 0) return fd;
        long n = sys(NR_PREAD64, fd, (long)buf, (long)len, (long)off, 0);
        l_close((int)fd);
        return n;
    }
    if (op != VR_LIST) return -30;                                   /* EROFS: read-only */
    long fd = l_open(p, 0200000);                                    /* O_DIRECTORY */
    if (fd < 0) return fd;
    static char dents[8192];
    u8 *out = buf;
    u64 o = 0;
    long n;
    while ((n = sys(NR_GETDENTS64, fd, (long)dents, sizeof dents, 0, 0)) > 0)
        for (long d = 0; d < n; d += *(u16 *)(dents + d + 16)) {
            const char *nm = dents + d + 19;
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            u8 st[128] = { 0 };
            if (sys(NR_NEWFSTATAT, fd, (long)nm, (long)st, 0x100, 0) < 0) continue;   /* AT_SYMLINK_NOFOLLOW */
            u32 mode = *(u32 *)(st + 16);
            u64 size = *(u64 *)(st + 48);
            usize l = MIN(strlen(nm), (usize)255);
            if (o + 12 + l > len) break;
            out[o] = (mode & 0170000) == 0040000;
            out[o + 1] = (u8)(mode & 0777); out[o + 2] = (u8)((mode & 0777) >> 8);
            memcpy(out + o + 3, &size, 8);
            out[o + 11] = (u8)l;
            memcpy(out + o + 12, nm, l);
            o += 12 + l;
        }
    l_close((int)fd);
    return (i64)o;
}
static int read_text(const char *path, char *out, int cap) {
    long fd = l_open(path, 0);
    if (fd < 0) return -1;
    long n = l_read((int)fd, out, cap - 1);
    l_close((int)fd);
    out[n > 0 ? n : 0] = 0;
    return (int)(n > 0 ? n : 0);
}
static void write_text(const char *path, const char *text) {
    long fd = sys(NR_OPENAT, AT_FDCWD, (long)path, 1, 0, 0);
    if (fd < 0) return;
    sys(64, fd, (long)text, (long)strlen(text), 0, 0);
    l_close((int)fd);
}
/* for lwifi.c: Linux's system calls, where a partition is, a remote processor started */
long argon_sys(long nr, long a, long b, long c, long d, long e) { return sys(nr, a, b, c, d, e); }
long argon_sys6(long nr, long a, long b, long c, long d, long e, long f) { long p[6] = { a, b, c, d, e, f }; return lkl_syscall(nr, p); }
const char *argon_part_path(const char *part) {
    for (int i = 0; i < namnt; i++) if (!strcmp(amnts[i].name, part)) return amnts[i].lpath;
    return NULL;
}
/* the remote processor whose firmware name starts with fw (wcnss: the Wi-Fi/Bluetooth
 * core): started through the secure world - 1 running, 0 none such, -1 failed */
int argon_remoteproc_start(const char *fw) {
    long fd = l_open("/sys/class/remoteproc", 0200000);
    if (fd < 0) return 0;
    static char d[2048];
    long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
    l_close((int)fd);
    for (long o = 0; o < n; o += *(u16 *)(d + o + 16)) {
        const char *nm = d + o + 19;
        if (strncmp(nm, "remoteproc", 10)) continue;
        char sp[96], st[32] = "", f[64] = "";
        fmt(sp, sizeof sp, "/sys/class/remoteproc/%s/firmware", nm);
        read_text(sp, f, sizeof f);
        if (!strstr(f, fw)) continue;                              /* "wcnss.mdt", or a path ending in it */
        fmt(sp, sizeof sp, "/sys/class/remoteproc/%s/state", nm);
        read_text(sp, st, sizeof st);
        if (!strncmp(st, "running", 7)) return 1;
        klog("argon: starting %s (%s) through the secure world", nm, fw);
        write_text(sp, "start");
        for (int t = 0; t < 100; t++) {                               /* up to 10 s */
            read_text(sp, st, sizeof st);
            if (!strncmp(st, "running", 7)) { klog("argon: %s running", nm); return 1; }
            thr_sleep_us(100000);
        }
        for (char *c = st; *c; c++) if (*c == '\n') *c = 0;
        klog("argon: %s did not start (state %s)", nm, st);
        return -1;
    }
    return 0;
}
static int mounted(const char *dev) { for (int i = 0; i < namnt; i++) if (strstr(amnts[i].lpath, dev)) return 1; return 0; }
static void firmware_from(const struct amnt *m) {
    char p[96];
    fmt(p, sizeof p, "%s/image", m->lpath);
    write_text("/sys/module/firmware_class/parameters/path", p);
    klog("argon: firmware from %s (the phone's own)", p);
    /* starting the modem, Wi-Fi and DSP goes through the secure world, which resets the
     * phone when it does not like what it is given - 0.21.5 and 0.21.6 went dark seconds
     * after the eMMC came up, as this ran.  Only with qrt.remoteproc on the command line. */
    int ch = fdt_node("/chosen"), al;
    const char *args = ch >= 0 ? fdt_prop(ch, "bootargs", &al) : NULL;
    int start = args && strstr(args, "qrt.remoteproc");
    if (!start) klog("argon: the modem, Wi-Fi and DSP stay off (qrt.remoteproc starts them)");
    long fd = l_open("/sys/class/remoteproc", 0200000);              /* the processors that wait for it */
    if (fd < 0) return;
    static char d[2048];
    long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
    l_close((int)fd);
    for (long o = 0; o < n; o += *(u16 *)(d + o + 16)) {
        const char *nm = d + o + 19;
        if (strncmp(nm, "remoteproc", 10)) continue;
        char sp[96], st[32], fw[64] = "";
        fmt(sp, sizeof sp, "/sys/class/remoteproc/%s/firmware", nm);
        read_text(sp, fw, sizeof fw);
        fmt(sp, sizeof sp, "/sys/class/remoteproc/%s/state", nm);
        read_text(sp, st, sizeof st);
        if (start && !strncmp(st, "offline", 7)) {
            klog("argon: starting %s (%s)", nm, fw);                 /* in the log kept across a reset */
            write_text(sp, "start");
            read_text(sp, st, sizeof st);
        }
        for (char *c = fw; *c; c++) if (*c == '\n') *c = 0;
        for (char *c = st; *c; c++) if (*c == '\n') *c = 0;
        klog("argon: %s (%s): %s", nm, fw, st);
    }
}
static void storage_scan(void) {
    long fd = l_open("/sys/class/block", 0200000);
    if (fd < 0) return;
    static char d[8192];
    long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
    l_close((int)fd);
    sys(NR_MKDIRAT, AT_FDCWD, (long)"/mnt", 0755, 0, 0);
    if (!vfs_lookup("/mnt")) vfs_create("/mnt", 1);
    for (long o = 0; o < n && namnt < NAMNT; o += *(u16 *)(d + o + 16)) {
        const char *dev = d + o + 19;
        if (dev[0] == '.' || mounted(dev)) continue;
        char ue[512], p[96], part[40] = "";
        fmt(p, sizeof p, "/sys/class/block/%s/uevent", dev);
        if (read_text(p, ue, sizeof ue) <= 0 || !strstr(ue, "DEVTYPE=partition")) continue;
        const char *pn = strstr(ue, "PARTNAME=");
        if (pn) { int i = 0; for (pn += 9; *pn && *pn != '\n' && i < 39; pn++) part[i++] = *pn; part[i] = 0; }
        struct amnt *m = &amnts[namnt];
        strlcpy(m->name, part[0] ? part : dev, sizeof m->name);
        fmt(m->lpath, sizeof m->lpath, "/mnt/%s", dev);
        fmt(p, sizeof p, "/dev/%s", dev);
        sys(NR_MKDIRAT, AT_FDCWD, (long)m->lpath, 0755, 0, 0);
        static const char *const fss[] = { "ext4", "f2fs", "vfat", "exfat", NULL };
        const char *got = NULL;
        for (int i = 0; fss[i] && !got; i++)
            if (sys(NR_MOUNT, (long)p, (long)m->lpath, (long)fss[i], 1, 0) == 0) got = fss[i];   /* MS_RDONLY */
        if (!got) continue;
        char tp[64];
        fmt(tp, sizeof tp, "/mnt/%s", m->name);
        if (vfs_lookup(tp)) fmt(tp, sizeof tp, "/mnt/%s-%s", m->name, dev);
        namnt++;
        vfs_mount_remote(tp, m);
        klog("argon: %s (%s, %s) at %s, read-only", dev, m->name, got, tp);
        if (!strncmp(m->name, "modem", 5)) firmware_from(m);
    }
}
static int scans;
static void storage_loop(void *a) {
    (void)a;
    vfs_remote_call = argon_vfs;
    for (;;) { if (linux_running()) { storage_scan(); scans++; } thr_sleep_us(10ull * 1000000); }
}

/* ---- settings kept across boots: on the phone, a file on the logdump partition (Xiaomi's
 * crash-log space, ext4 - nothing of Android's lives there).  That one partition is
 * remounted writable; the rest stay read-only.  Wi-Fi on/off, the saved network, the
 * brightness... (hal.c keeps them in RAM and counts the changes) ---- */
#define NR_WRITE    64
#define NR_RENAMEAT 38
#define NR_FSYNC    82
u32 hal_settings_gen(void);
usize hal_settings_export(u8 *buf, usize cap);
int hal_settings_import(const u8 *buf, usize len);
void hal_settings_mark_ready(void);
void backlight_restore(void);
static void settings_ready(void) { hal_settings_mark_ready(); backlight_restore(); }   /* the saved brightness, now */
static void settings_loop(void *a) {
    (void)a;
    const struct amnt *m = NULL;
    while (!m) {                                                    /* the partition, once mounted */
        for (int i = 0; i < namnt && !m; i++) if (!strcmp(amnts[i].name, "logdump")) m = &amnts[i];
        if (m) break;
        if (dead || scans >= 2) { klog("settings: no logdump partition - settings last until power-off"); settings_ready(); return; }
        thr_sleep_us(1000000);
    }
    char dir[96], file[112], tmp[112];
    fmt(dir, sizeof dir, "%s/qrt", m->lpath);
    fmt(file, sizeof file, "%s/settings", dir);
    fmt(tmp, sizeof tmp, "%s/settings.new", dir);
    long r = sys(NR_MOUNT, 0, (long)m->lpath, 0, 32, 0);            /* MS_REMOUNT, writable */
    if (r < 0) { klog("settings: logdump stays read-only (%ld) - settings last until power-off", r); settings_ready(); return; }
    sys(NR_MKDIRAT, AT_FDCWD, (long)dir, 0755, 0, 0);
    static u8 buf[64 * 300];
    long fd = l_open(file, 0);
    if (fd >= 0) {
        long n = l_read((int)fd, buf, sizeof buf);
        l_close((int)fd);
        int got = n > 8 && !memcmp(buf, "QRTSET1\n", 8) ? hal_settings_import(buf + 8, (usize)(n - 8)) : 0;
        klog("settings: %d loaded from the phone (logdump/qrt/settings)", got);
    } else klog("settings: none saved yet (logdump/qrt/settings)");
    settings_ready();
    u32 saved = hal_settings_gen() ? ~0u : 0;                       /* set before the file was read: save them too */
    for (;;) {
        thr_sleep_us(2000000);
        u32 g = hal_settings_gen();
        if (g == saved) continue;
        memcpy(buf, "QRTSET1\n", 8);
        usize n = 8 + hal_settings_export(buf + 8, sizeof buf - 8);
        fd = sys(NR_OPENAT, AT_FDCWD, (long)tmp, 01101, 0600, 0);    /* O_WRONLY | O_CREAT | O_TRUNC */
        if (fd < 0) { klog("settings: could not save (%ld)", fd); saved = g; continue; }
        long w = sys(NR_WRITE, fd, (long)buf, (long)n, 0, 0);
        sys(NR_FSYNC, fd, 0, 0, 0, 0);
        l_close((int)fd);
        if (w == (long)n && sys(NR_RENAMEAT, AT_FDCWD, (long)tmp, AT_FDCWD, (long)file, 0) == 0) saved = g;
        else { klog("settings: could not save (%ld)", w); saved = g; }
    }
}

/* ---- Argon power: Linux's fuel gauge and charger (PMI8950 on the Mi A1) as Tessera's
 * battery (the top bar, Settings) ---- */
#include "../../drivers/battery.h"
static battery_t lbat = { .minutes = -1 };
const battery_t *linux_battery(void) { return lbat.present ? &lbat : NULL; }
static long num_at(const char *dir, const char *f) {
    char p[128], t[32];
    fmt(p, sizeof p, "/sys/class/power_supply/%s/%s", dir, f);
    if (read_text(p, t, sizeof t) <= 0) return -1;
    long v = 0, sign = 1;
    const char *c = t;
    if (*c == '-') { sign = -1; c++; }
    while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0');
    return v * sign;
}
static void power_poll(void) {
    long fd = l_open("/sys/class/power_supply", 0200000);
    if (fd < 0) return;
    static char d[2048];
    long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
    l_close((int)fd);
    battery_t b = { .minutes = -1 };
    for (long o = 0; o < n; o += *(u16 *)(d + o + 16)) {
        const char *nm = d + o + 19;
        if (nm[0] == '.') continue;
        char p[128], type[24] = "", st[24] = "";
        fmt(p, sizeof p, "/sys/class/power_supply/%s/type", nm);
        read_text(p, type, sizeof type);
        if (!strncmp(type, "Battery", 7)) {
            long cap = num_at(nm, "capacity");
            if (cap < 0) continue;
            b.present = 1;
            b.percent = (int)MIN(100, MAX(0, cap));
            fmt(p, sizeof p, "/sys/class/power_supply/%s/status", nm);
            read_text(p, st, sizeof st);
            b.charging = !strncmp(st, "Charging", 8);
            b.discharging = !strncmp(st, "Discharging", 11);
            long uv = num_at(nm, "voltage_now"), ua = num_at(nm, "current_now");
            if (uv > 0) b.mv = (int)(uv / 1000);
            if (ua != -1) b.ma = (int)((ua < 0 ? -ua : ua) / 1000);
            b.critical = b.discharging && b.percent <= 3;
        } else if (num_at(nm, "online") > 0) b.ac = 1;                 /* USB or mains with power */
    }
    if (b.present) lbat = b;
}
/* no battery after a minute: say why - is the gauge's driver there, did it take the
 * device, which supplies Linux has, is the device still waiting for something */
static void battery_why(void) {
    char line[200] = "", p[96];
    long fd = l_open("/sys/bus/platform/drivers/qcom-fg", 0200000);
    int bound = -1;
    if (fd >= 0) {
        static char d[1024];
        long len = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
        bound = 0;
        for (long o = 0; o < len; o += *(u16 *)(d + o + 16)) if (strchr(d + o + 19, ':')) bound++;
        l_close((int)fd);
    }
    fd = l_open("/sys/class/power_supply", 0200000);
    if (fd >= 0) {
        static char d[1024];
        long len = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
        for (long o = 0; o < len; o += *(u16 *)(d + o + 16)) {
            const char *nm = d + o + 19;
            if (nm[0] == '.') continue;
            char cap[16] = "?";
            fmt(p, sizeof p, "/sys/class/power_supply/%s/capacity", nm);
            if (read_text(p, cap, sizeof cap) <= 0) fmt(cap, sizeof cap, "unreadable");
            for (char *t = cap; *t; t++) if (*t == '\n') *t = 0;
            usize l = strlen(line);
            fmt(line + l, sizeof line - l, " %s (capacity %s)", nm, cap);
        }
        l_close((int)fd);
    }
    int found = 0;                                                    /* is there a gauge device at all */
    fd = l_open("/sys/bus/platform/devices", 0200000);
    if (fd >= 0) {
        static char d[8192];
        long len;
        while ((len = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0)) > 0)
            for (long o = 0; o < len; o += *(u16 *)(d + o + 16)) if (strstr(d + o + 19, "fuel-gauge")) found = 1;
        l_close((int)fd);
    }
    klog("argon: no battery: the gauge device %s", found ? "exists" : "was never made (its PMIC's children not populated)");
    for (int i = nfg > 3 ? nfg - 3 : 0; i < nfg; i++) klog("argon: gauge said: %s", fgl[i % 3]);
    if (!nfg) klog("argon: the gauge driver said nothing - its probe never ran");
    fd = l_open("/sys/kernel/debug/devices_deferred", 0);               /* still waiting for something? */
    if (fd >= 0) {
        static char buf[4096];
        long len = l_read((int)fd, buf, sizeof buf - 1);
        l_close((int)fd);
        buf[len > 0 ? len : 0] = 0;
        for (char *l = buf; *l; ) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            if (strstr(l, "fuel")) { for (char *t = l; *t; t++) if (*t == '\t') *t = ' '; klog("argon: gauge waits: %s", l); }
            if (!e) break;
            l = e + 1;
        }
    }
    klog("argon: no battery: gauge driver %s, %d device(s); supplies:%s", bound < 0 ? "missing" : "present", bound < 0 ? 0 : bound, line[0] ? line : " none");
}
static void sensors_loop(void *a) {
    (void)a;
    int said = 0;
    for (;;) {
        if (linux_running()) {
            power_poll();
            if (!said && lbat.present) { said = 1; klog("argon: battery %d%%%s (Linux's fuel gauge)", lbat.percent, lbat.charging ? ", charging" : ""); }
            static int polls;
            if (!said && !lbat.present && ++polls == 12) battery_why();      /* a minute and no battery */
        }
        thr_sleep_us(5ull * 1000000);
    }
}


/* ---- Argon sensors and small parts (the Mi A1): which way up (the BMI120 accelerometer,
 * Linux's bmi160 driver over IIO), how bright around (the LTR579 light sensor - no Linux
 * driver: read here through /dev/i2c-N), the notification LED (AW2013, Linux's leds
 * class) and the vibration motor (PMI8950's haptics, Linux's input force feedback) ---- */
#define NR_WRITE2 64
static char accel_dir[64];
static int mmat[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
static volatile int orient = -1, lux_now = -1;
static char sens_status[80] = "none";
int ish_orientation(void) { return orient; }
int ish_lux(void) { return lux_now; }
const char *ish_status(void) { return sens_status; }
static long read_long(const char *p) {
    char t[32];
    if (read_text(p, t, sizeof t) <= 0) return -999999;
    long v = 0, sg = 1;
    const char *c = t;
    if (*c == '-') { sg = -1; c++; }
    if (*c < '0' || *c > '9') return -999999;
    while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0');
    return v * sg;
}
static void find_accel(void) {
    long fd = l_open("/sys/bus/iio/devices", 0200000);
    if (fd < 0) return;
    static char d[2048];
    long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
    l_close((int)fd);
    for (long o = 0; o < n && !accel_dir[0]; o += *(u16 *)(d + o + 16)) {
        const char *nm = d + o + 19;
        if (strncmp(nm, "iio:device", 10)) continue;
        char p[96], name[32] = "";
        fmt(p, sizeof p, "/sys/bus/iio/devices/%s/in_accel_x_raw", nm);
        if (read_long(p) == -999999) continue;
        fmt(accel_dir, sizeof accel_dir, "/sys/bus/iio/devices/%s", nm);
        fmt(p, sizeof p, "%s/name", accel_dir);
        read_text(p, name, sizeof name);
        for (char *c = name; *c; c++) if (*c == '\n') *c = 0;
        char m[96] = "";
        fmt(p, sizeof p, "%s/in_accel_mount_matrix", accel_dir);
        if (read_text(p, m, sizeof m) <= 0) { fmt(p, sizeof p, "%s/mount_matrix", accel_dir); read_text(p, m, sizeof m); }
        int k = 0;
        for (const char *c = m; *c && k < 9; ) {                         /* "0, 1, 0; 1, 0, 0; 0, 0, 1" */
            while (*c && *c != '-' && (*c < '0' || *c > '9')) c++;
            if (!*c) break;
            int sg = 1;
            if (*c == '-') { sg = -1; c++; }
            int v = 0;
            while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0');
            if (*c == '.') { c++; while (*c >= '0' && *c <= '9') c++; }
            mmat[k++] = sg * v;
        }
        fmt(sens_status, sizeof sens_status, "accelerometer (%s)", name[0] ? name : nm);
        klog("argon: accelerometer %s (%s), mount matrix %d %d %d / %d %d %d / %d %d %d", name, nm,
             mmat[0], mmat[1], mmat[2], mmat[3], mmat[4], mmat[5], mmat[6], mmat[7], mmat[8]);
    }
}
/* the screen turn that keeps the picture upright (the shell's rotation): the axis gravity
 * pulls along, once it clearly does - flat on a table keeps what was */
static void accel_poll(void) {
    char p[96];
    long r[3];
    static const char ax[3] = { 'x', 'y', 'z' };
    for (int i = 0; i < 3; i++) {
        fmt(p, sizeof p, "%s/in_accel_%c_raw", accel_dir, ax[i]);
        r[i] = read_long(p);
        if (r[i] == -999999) return;
    }
    long v[3];
    for (int i = 0; i < 3; i++) v[i] = mmat[3 * i] * r[0] + mmat[3 * i + 1] * r[1] + mmat[3 * i + 2] * r[2];
    /* the screen's axes (x to its right edge, y to its top) from the matrix's: on the Mi A1
     * (0.25.0, turning it by hand) the matrix's second axis follows the left and right edges
     * and its first the top and bottom - tilting left gave -v[1], right +v[1] */
    long sx = -v[1], sy = -v[0];
    long ax_ = sx < 0 ? -sx : sx, ay = sy < 0 ? -sy : sy, az = v[2] < 0 ? -v[2] : v[2];
    int o = -1;
    if (az > 2 * (ax_ > ay ? ax_ : ay)) o = -1;                       /* flat */
    else if (ay * 2 > ax_ * 3) o = sy > 0 ? 0 : 2;                    /* top up: as it is; top down: upside down */
    else if (ax_ * 2 > ay * 3) o = sx > 0 ? 1 : 3;                    /* right edge up (turned left): 90 */
    static int last = -1, same;
    if (o == last) same++; else { last = o; same = 0; }
    if (o >= 0 && same >= 1 && o != orient) {
        orient = o;
        static int said;
        if (said++ < 8) klog("argon: turned: %d degrees (accelerometer %ld %ld %ld)", o * 90, v[0], v[1], v[2]);
    }
}
/* LTR579 on the sensors' I2C (gpio) bus, through Linux's i2c-dev */
static long ltr_fd = -1;
static int ltr_xfer(u8 reg, u8 *rd, int rn, int wv) {
    u8 w[2] = { reg, (u8)wv };
    struct { u16 addr, flags, len, pad; u8 *buf; } msgs[2] = {
        { 0x53, 0, (u16)(wv >= 0 ? 2 : 1), 0, w }, { 0x53, 1, (u16)rn, 0, rd } };
    struct { void *msgs; u32 n; } x = { msgs, wv >= 0 ? 1u : 2u };
    return sys(NR_IOCTL, ltr_fd, 0x0707, (long)&x, 0, 0) < 0 ? -1 : 0;   /* I2C_RDWR */
}
static void find_light(void) {
    for (int b = 0; b < 16; b++) {
        char p[80], nm[32] = "";
        fmt(p, sizeof p, "/sys/bus/i2c/devices/i2c-%d/of_node/name", b);
        if (read_text(p, nm, sizeof nm) <= 0 || strncmp(nm, "i2c-sensors", 11)) continue;
        fmt(p, sizeof p, "/dev/i2c-%d", b);
        ltr_fd = l_open(p, 2);
        if (ltr_fd < 0) { klog("argon: light sensor: %s did not open (%ld)", p, ltr_fd); return; }
        u8 id = 0;
        if (ltr_xfer(0x06, &id, 1, -1) < 0) { klog("argon: light sensor: no answer at 0x53"); l_close((int)ltr_fd); ltr_fd = -1; return; }
        ltr_xfer(0x05, NULL, 0, 0x01);                               /* ALS_GAIN: x3 */
        ltr_xfer(0x04, NULL, 0, 0x22);                               /* ALS_MEAS_RATE: 18 bits, 100 ms */
        ltr_xfer(0x00, NULL, 0, 0x02);                               /* MAIN_CTRL: ALS on */
        klog("argon: light sensor LTR579 (part %02x) on i2c-%d", id, b);
        return;
    }
}
static void light_poll(void) {
    u8 d[3];
    if (ltr_fd < 0 || ltr_xfer(0x0d, d, 3, -1) < 0) return;
    long raw = d[0] | d[1] << 8 | (d[2] & 0x0f) << 16;
    lux_now = (int)(raw / 5);                                         /* x 0.6 / gain 3 / 100 ms */
    if (!hal_setting_get(u"QrtAutoBright", 1)) return;
    /* about 8 + 22 x log10(1 + lux): dark 10 %, a room 50 %, daylight 100 % */
    static const u32 dec[] = { 1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25, 32, 40, 50, 63, 79, 100, 126, 158, 200, 251, 316, 398, 501, 631,
                               794, 1000, 1259, 1585, 1995, 2512, 3162, 3981, 5012, 6310, 7943, 10000, 12589, 15849, 19953, 25119,
                               31623, 39811, 50119 };   /* 10^(k/10) */
    int lg = 0;                                                       /* log10(1 + lux), in tenths */
    while (lg + 1 < (int)ARRAY_LEN(dec) && (u32)(lux_now + 1) >= dec[lg + 1]) lg++;
    int want = CLAMP(8 + 22 * lg / 10, 8, 100);
    static int cur = -1;
    if (cur < 0) cur = want;
    cur += (want - cur) / 3;                                          /* settle over a few seconds */
    static int set = -1;
    if (set < 0 || cur - set > 2 || set - cur > 2) { set = cur; void backlight_auto_level(int); backlight_auto_level(cur); }
}
/* the notification LED: on while charging, blinking when the battery runs low */
static char led_dir[80];
static void led_poll(void) {
    if (!led_dir[0]) {
        long fd = l_open("/sys/class/leds", 0200000);
        if (fd < 0) return;
        static char d[1024];
        long n = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
        l_close((int)fd);
        for (long o = 0; o < n; o += *(u16 *)(d + o + 16))
            if (strstr(d + o + 19, "indicator")) { fmt(led_dir, sizeof led_dir, "/sys/class/leds/%s", d + o + 19); klog("argon: notification LED %s", d + o + 19); break; }
        if (!led_dir[0]) { strlcpy(led_dir, "-", sizeof led_dir); return; }
    }
    if (led_dir[0] == '-' || !lbat.present) return;
    int want = lbat.charging && lbat.percent < 100 ? 1 : lbat.discharging && lbat.percent <= 15 ? 2 : 0;
    static int now = -1;
    if (want == now) return;
    now = want;
    char p[100];
    fmt(p, sizeof p, "%s/trigger", led_dir);
    write_text(p, want == 2 ? "timer" : "none");
    fmt(p, sizeof p, "%s/brightness", led_dir);
    write_text(p, want ? "255" : "0");
}
/* vibration: a short buzz on request (the keyboard's keys) */
static long vib_fd = -1;
static int vib_id = -1;
static volatile int vib_ms;
static sem_t *vib_sem;
void hal_vibrate(int ms) { if (vib_fd >= 0 && vib_sem) { vib_ms = ms; sem_up(vib_sem); } }
static void find_vibrator(void) {
    for (int i = 0; i < 16; i++) {
        char p[80], nm[48] = "";
        fmt(p, sizeof p, "/sys/class/input/event%d/device/name", i);
        if (read_text(p, nm, sizeof nm) <= 0 || !strstr(nm, "haptics")) continue;
        fmt(p, sizeof p, "/dev/input/event%d", i);
        vib_fd = l_open(p, 2);
        klog("argon: vibration motor (%s, event%d)%s", "PMI8950 haptics", i, vib_fd < 0 ? " did not open" : "");
        return;
    }
}
static void vib_loop(void *a) {
    (void)a;
    for (;;) {
        sem_down(vib_sem);                                             /* until a buzz is asked for */
        int ms = vib_ms;
        if (!ms) continue;
        vib_ms = 0;
        u8 eff[48];
        memset(eff, 0, sizeof eff);
        *(u16 *)eff = 0x50;                                            /* FF_RUMBLE */
        *(i16 *)(eff + 2) = (i16)vib_id;
        *(u16 *)(eff + 10) = (u16)ms;                                  /* replay.length */
        *(u16 *)(eff + 16) = 0xc000;                                   /* rumble.strong_magnitude */
        if (sys(NR_IOCTL, vib_fd, 0x40304580, (long)eff, 0, 0) < 0) continue;   /* EVIOCSFF */
        vib_id = *(i16 *)(eff + 2);
        u8 ev[24];
        memset(ev, 0, sizeof ev);
        *(u16 *)(ev + 16) = 0x15; *(u16 *)(ev + 18) = (u16)vib_id; *(i32 *)(ev + 20) = 1;   /* EV_FF, play */
        sys(NR_WRITE2, vib_fd, (long)ev, sizeof ev, 0, 0);
    }
}
static void motion_loop(void *a) {
    (void)a;
    for (int t = 0; t < 120 && !accel_dir[0]; t++) { if (linux_running()) find_accel(); if (!accel_dir[0]) thr_sleep_us(1000000); }
    if (!accel_dir[0]) { strlcpy(sens_status, "none found in Linux", sizeof sens_status); }
    find_light();
    find_vibrator();
    if (vib_fd >= 0) { vib_sem = sem_new(0); thr_create("vibration", vib_loop, NULL, 16 << 10); }
    int shell_auto_rotate(void);
    for (u64 n = 0;; n++) {                                           /* every half second */
        if (accel_dir[0] && shell_auto_rotate()) accel_poll();        /* only what is used */
        if (n % 6 == 0 && hal_setting_get(u"QrtAutoBright", 1)) light_poll();
        if (n % 10 == 0) led_poll();
        thr_sleep_us(500000);
    }
}

/* ---- start ---- */
static const void *dtb;
/* what did not come up and why: the devices still waiting for something (debugfs's
 * devices_deferred, with the reason each driver gave) and Linux's error lines */
static void linux_summary(void) {
    sys(NR_MOUNT, (long)"debugfs", (long)"/sys/kernel/debug", (long)"debugfs", 0, 0);
    long fd = l_open("/sys/kernel/debug/devices_deferred", 0);
    int n = 0;
    if (fd >= 0) {
        static char buf[4096];
        long len = l_read((int)fd, buf, sizeof buf - 1);
        l_close((int)fd);
        buf[len > 0 ? len : 0] = 0;
        for (char *l = buf; *l; ) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            for (char *t = l; *t; t++) if (*t == '\t') *t = ' ';
            if (*l) { klog("linux: waiting: %s", l); n++; }
            if (!e) break;
            l = e + 1;
        }
    }
    int nreg = 0;
    fd = l_open("/sys/class/regulator", 0200000);
    if (fd >= 0) {
        static char d[2048];
        long len = sys(NR_GETDENTS64, fd, (long)d, sizeof d, 0, 0);
        for (long o = 0; o < len; o += *(u16 *)(d + o + 16)) if (d[o + 19] != '.') nreg++;
        l_close((int)fd);
    }
    /* the drivers the touch screen and storage need, and how many devices each has */
    static const char *const drv[] = { "qcom_rpm_smd_regulator", "qcom-clk-smd-rpm", "qcom-rpmpd", "gcc-msm8953", "i2c_qup", "sdhci_msm", NULL };
    char line[160] = "", p[96];
    for (int i = 0; drv[i]; i++) {
        fmt(p, sizeof p, "/sys/bus/platform/drivers/%s", drv[i]);
        int bound = -1;
        long dfd = l_open(p, 0200000);
        if (dfd >= 0) {
            static char d[1024];
            long len = sys(NR_GETDENTS64, dfd, (long)d, sizeof d, 0, 0);
            bound = 0;
            for (long o = 0; o < len; o += *(u16 *)(d + o + 16)) if (d[o + 19] != '.' && (strchr(d + o + 19, '.') || strchr(d + o + 19, ':'))) bound++;
            l_close((int)dfd);
        }
        klog("linux: driver %s: %d device(s)%s", drv[i], bound < 0 ? 0 : bound, bound < 0 ? " (no such driver)" : "");
    }
    (void)line;
    {                                                             /* the busiest device interrupts */
        char il[160] = "";
        static u8 shown[1024];
        memset(shown, 0, sizeof shown);
        for (int k2 = 0; k2 < 8; k2++) {
            int best = -1;
            for (int i = 32; i < 1024; i++) if (!shown[i] && irq_count[i] && (best < 0 || irq_count[i] > irq_count[best])) best = i;
            if (best < 0) break;
            shown[best] = 1;
            usize l = strlen(il);
            fmt(il + l, sizeof il - l, "%s%d:%u", k2 ? " " : "", best - 32, irq_count[best]);
        }
        klog("linux: interrupts delivered (SPI:count): %s", il[0] ? il : "none");
    }
    {                                                             /* the boot's display lines, again */
        int nl = 0, shown = 0;
        while (klog_line(nl)) nl++;
        for (int i = 0; i < nl && shown < 12; i++) {
            const char *l = klog_line(i);
            if (l && (!strncmp(l, "display:", 8) || !strncmp(l, "gic:", 4))) { char c[160]; strlcpy(c, l, sizeof c); klog("%s", c); shown++; }
        }
    }
    klog("linux: %llu interrupt run(s) on top of spinning Linux code (not: %llu off, %llu nested, %llu none, %llu not owner)", tail_runs, tail_why[1], tail_why[2], tail_why[3], tail_why[4]);
    klog("linux: %d regulator(s); %d device(s) waiting; %d line(s) to note:", nreg, n, nprob);
    for (int i = 0; i < nprob; i++) klog("linux: ! %s", probs[i]);
}

static void linux_main(void *a) {
    (void)a;
    lkl_qrt_fdt = (void *)dtb;
    lkl_qrt_dma_base = arm_dma_pool_base;
    lkl_qrt_dma_size = arm_dma_pool_size;
    plog_state(PLOG_LINUX_STARTING);                                /* a reset from here on: the next boot is safe */
    if (lkl_init(&ops) < 0) { klog("linux: lkl_init failed"); return; }
    lkl_up = 1;
    /* mac80211_hwsim (virtual radios, lwifi.c's QEMU test) only in QEMU */
    int qemu = fdt_find_compatible(-1, "linux,dummy-virt") >= 0;
    int r = lkl_start_kernel(qemu ? "mem=160M loglevel=8 initcall_debug clk_ignore_unused pd_ignore_unused "
                                    "regulator_ignore_unused fw_devlink=permissive"
                                  : "mem=160M loglevel=8 initcall_debug clk_ignore_unused pd_ignore_unused "
                                    "regulator_ignore_unused fw_devlink=permissive mac80211_hwsim.radios=0 snd_dummy.enable=0");
    if (r < 0) { klog("linux: did not start (%d)", r); return; }
    plog_state(PLOG_LINUX_OK);                                      /* up: a later reset is not its start's */
    running = 1;
    klog("linux: running; %d threads", thr_count());
    sys(NR_MKDIRAT, AT_FDCWD, (long)"/sys", 0755, 0, 0);
    sys(NR_MKDIRAT, AT_FDCWD, (long)"/dev", 0755, 0, 0);
    sys(NR_MOUNT, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
    sys(NR_MOUNT, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
    thr_create("evdev scan", input_scan, NULL, 32 << 10);
    thr_create("backlight", bl_loop, NULL, 32 << 10);
    thr_create("linux display", display_loop, NULL, 32 << 10);
    thr_create("linux storage", storage_loop, NULL, 64 << 10);
    thr_create("settings", settings_loop, NULL, 32 << 10);
    void lsound_start(void);
    lsound_start();                                                 /* the audio DSP and the speaker */
    void lbt_start(void);
    lbt_start();                                                    /* Bluetooth: hci0 to QRT's own stack */
    thr_create("linux sensors", sensors_loop, NULL, 32 << 10);
    if (fdt_find_compatible(-1, "linux,dummy-virt") < 0) thr_create("motion", motion_loop, NULL, 32 << 10);
    thr_sleep_us(25ull * 1000000);
    if (!dead) linux_summary();
    thr_sleep_us(5ull * 1000000);
    if (!have_touch && !dead) {                                     /* no touch screen: the log says why */
        klog("linux: no touch screen 30 s after boot - showing the log (volume up x3 closes it)");
        void logview_show_current(void);
        logview_show_current();
    }
    thr_sleep_us(10ull * 1000000);                                  /* settled: probes, deferred probes, sync_state */
    if (!dead) klog("linux: drivers settled");
}

int linux_start(const void *fdt) {
    dtb = fdt;
    {
        int ch = fdt_node("/chosen"), al;
        const char *args = ch >= 0 ? fdt_prop(ch, "bootargs", &al) : NULL;
        void lwifi_test_boot(int wpa);
        if (args && strstr(args, "qrt.wifitest")) lwifi_test_boot(strstr(args, "qrt.wifitest=wpa") != NULL);
    }
    void fb_linux_owns_mdp(void);
    fb_linux_owns_mdp();
    timer_thr = thr_create("linux timers", timer_loop, NULL, 64 << 10);
    thr_create("linux irqs", irq_loop, NULL, 64 << 10);
    thr_create("linux", linux_main, NULL, 128 << 10);
    return 1;
}
const char *linux_status(char *buf, int cap) {
    fmt(buf, (usize)cap, dead ? "stopped - see the log below" : running ? "running: %d threads, %llu interrupts" : "starting", thr_count(), nirq);
    return buf;
}
#else
void argon_irq_tail(u64 pc) { (void)pc; }
int linux_has_keys(void) { return 0; }
#include "../../drivers/battery.h"
const battery_t *linux_battery(void) { return NULL; }
void linux_failed(const char *why) { (void)why; }
int linux_backlight_set(int pct) { (void)pct; return -1; }
int linux_backlight_present(void) { return 0; }
int linux_start(const void *fdt) { (void)fdt; return 0; }
int linux_running(void) { return 0; }
int linux_input_poll(event_t *out, int max) { (void)out; (void)max; return 0; }
const char *linux_status(char *buf, int cap) { strlcpy(buf, "not in this build", (usize)cap); return buf; }
int ish_orientation(void) { return -1; }
int ish_lux(void) { return -1; }
const char *ish_status(void) { return "none"; }
void hal_vibrate(int ms) { (void)ms; }
#endif
