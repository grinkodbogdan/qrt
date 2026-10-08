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
void linux_failed(const char *why) { dead = 1; klog("linux: stopped (%s); the shell goes on - this log is in System", why); }

/* ---- the host operations ---- */
static char pline[256];
static int plen;
static void h_print(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n' || plen == (int)sizeof pline - 1) { pline[plen] = 0; if (plen) klog("linux: %s", pline); plen = 0; }
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
static unsigned long long h_time(void) { return k_now_us() * 1000ull; }
static void *h_ioremap(long addr, int size) { (void)size; return (void *)(usize)addr; }
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
static thr_t *timer_thr;
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
    .jmp_buf_set = h_jmp_set, .jmp_buf_longjmp = h_jmp_long,
    .memcpy = h_memcpy, .memset = h_memset, .memmove = h_memmove,
};

/* ---- interrupts: Linux's SPIs, pending and enabled in the distributor (gic.c) ---- */
static u64 nirq;
int gic_pending(void (*fn)(u32 id));
static void deliver(u32 id) { lkl_trigger_irq(1024 + (int)id); nirq++; }
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
        if (n <= 0) { if (n == -19) break; thr_sleep_us(10000); continue; }      /* -ENODEV: unplugged */
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
                int axis = e->code == 0 ? 0 : e->code == 1 ? 1 : -1;
                if (e->code == 0x39) fingers += e->value >= 0 ? 1 : -1;  /* ABS_MT_TRACKING_ID */
                if (axis < 0) continue;
                int span = d->max[axis] - d->min[axis];
                int px = span > 0 ? (int)((i64)(e->value - d->min[axis]) * ((axis ? (int)k.fb_h : (int)k.fb_w) - 1) / span) : 0;
                if (axis) y = px; else x = px;
                moved = 1;
            } else if (e->type == 0 && e->code == 0 && d->abs) {         /* SYN_REPORT */
                int f = fingers > 0 ? fingers : touch;
                if (touch && !was) { event_t t = { .type = EV_DOWN, .x = x, .y = y, .fingers = f }; inq_put(t); }
                else if (touch && moved) { event_t t = { .type = EV_MOVE, .x = x, .y = y, .fingers = f }; inq_put(t); }
                else if (!touch && was) { event_t t = { .type = EV_UP, .x = x, .y = y, .fingers = 0 }; inq_put(t); }
                was = touch; moved = 0;
                if (fingers < 0) fingers = 0;
            }
        }
    }
    klog("linux: %s went away", d->name);
    l_close(d->fd);
    d->open = 0;
}

static struct indev indevs[8];
static int have_keys, have_touch;
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

/* ---- start ---- */
static const void *dtb;
static void linux_main(void *a) {
    (void)a;
    lkl_qrt_fdt = (void *)dtb;
    lkl_qrt_dma_base = arm_dma_pool_base;
    lkl_qrt_dma_size = arm_dma_pool_size;
    plog_state(PLOG_LINUX_STARTING);                                /* a reset from here on: the next boot is safe */
    if (lkl_init(&ops) < 0) { klog("linux: lkl_init failed"); return; }
    int r = lkl_start_kernel("mem=160M loglevel=8 initcall_debug clk_ignore_unused pd_ignore_unused "
                             "regulator_ignore_unused fw_devlink=permissive");
    if (r < 0) { klog("linux: did not start (%d)", r); return; }
    running = 1;
    klog("linux: running; %d threads", thr_count());
    sys(NR_MKDIRAT, AT_FDCWD, (long)"/sys", 0755, 0, 0);
    sys(NR_MKDIRAT, AT_FDCWD, (long)"/dev", 0755, 0, 0);
    sys(NR_MOUNT, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
    sys(NR_MOUNT, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
    thr_create("evdev scan", input_scan, NULL, 32 << 10);
    thr_create("backlight", bl_loop, NULL, 32 << 10);
    thr_sleep_us(30ull * 1000000);
    if (!have_touch && !dead) {                                     /* no touch screen: the log says why */
        klog("linux: no touch screen 30 s after boot - showing the log (volume up x3 closes it)");
        void logview_show_current(void);
        logview_show_current();
    }
    thr_sleep_us(10ull * 1000000);                                  /* settled: probes, deferred probes, sync_state */
    if (!dead) { plog_state(PLOG_LINUX_OK); klog("linux: drivers settled"); }
}

int linux_start(const void *fdt) {
    dtb = fdt;
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
int linux_has_keys(void) { return 0; }
void linux_failed(const char *why) { (void)why; }
int linux_backlight_set(int pct) { (void)pct; return -1; }
int linux_backlight_present(void) { return 0; }
int linux_start(const void *fdt) { (void)fdt; return 0; }
int linux_running(void) { return 0; }
int linux_input_poll(event_t *out, int max) { (void)out; (void)max; return 0; }
const char *linux_status(char *buf, int cap) { strlcpy(buf, "not in this build", (usize)cap); return buf; }
#endif
