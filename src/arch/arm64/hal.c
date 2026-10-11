/*
 * hal.c - the platform services the shell uses (kernel.h's hal_*), on 64-bit ARM:
 * display (fb.c), input (virtio.c, msm.c, the console), frame timing (the generic
 * timer), settings (kept in RAM: no NVRAM yet), the clock (PL031 RTC on QEMU), power
 * (PSCI SYSTEM_OFF / SYSTEM_RESET through the method the device tree names).
 */
#include "arm.h"
#include "sched.h"
#include "../../ui/shell.h"

static int cur_x, cur_y, cur_visible;
static int psci_hvc = -1;                    /* -1: no PSCI; 0 smc; 1 hvc */
static u64 pl031;

void hal_arm_init(void) {
    int n = fdt_find_compatible(-1, "arm,psci-1.0");
    if (n < 0) n = fdt_find_compatible(-1, "arm,psci-0.2");
    int len;
    const char *m = n >= 0 ? fdt_prop(n, "method", &len) : NULL;
    if (m) psci_hvc = !strcmp(m, "hvc");
    n = fdt_find_compatible(-1, "arm,pl031");
    u64 a, s;
    if (n >= 0 && fdt_reg(n, 0, &a, &s)) pl031 = a;
#ifndef QRT_LKL
    virtio_input_init();                       /* with Linux built in, Linux's virtio drivers have them */
#endif
    msm_init();
    cur_x = (int)k.fb_w / 2; cur_y = (int)k.fb_h / 2;
}

/* ---- display ---- */
int  logview_active(void);
int  logview_key(void);
int  logview_expire(void);
void logview_draw(void);
void hal_present(const u32 *px, int stride, int x, int y, int w, int h) {
    if (logview_active()) { logview_draw(); return; }
    fb_present(px, stride, x, y, w, h);
}
void hal_cursor(int *x, int *y, int *visible) { *x = cur_x; *y = cur_y; *visible = cur_visible; }
void hal_set_touch_map(u32 map) { k.touch_map = map; }
void hal_reprobe_input(void) {}

/* the console: typed characters as keys (the QEMU test types on the serial line too) */
static int serial_keys(event_t *out, int max) {
    int n = 0, c;
    while (n < max && (c = uart_getc()) >= 0) {
        event_t e = { .type = EV_KEY };
        if (c == '\r' || c == '\n') e.ch = '\r';
        else if (c == 127) e.ch = 8;
        else e.ch = (c16)c;
        out[n++] = e;
    }
    return n;
}

/* ---- the shell's watchdog: when the shell has not come back for 3 s, the log (kept
 * across a reset: System -> the last boot's log) says where it is and who holds what ---- */
u64 arm_main_irq_at[3];                                       /* pc, lr, fp at the last tick that interrupted it */
static volatile u64 last_poll;
static void watchdog(void *a) {
    (void)a;
    int told = 0;
    extern char _start[];
    u64 b0 = (u64)(usize)_start;
    for (;;) {
        thr_sleep_us(1000000);
        u64 lp = last_poll, now = k_now_ms();
        if (!lp || now - lp < 3000) { told = 0; continue; }
        if (told++ >= 2) continue;                            /* twice per stall: the second shows if it moved */
        thr_t *all[96];
        int n = thr_list(all, 96);
        thr_t *m = all[0];                                    /* the ring starts at the shell's */
        char chain[160] = "";
        usize cl = 0;
        u64 fp = arm_main_irq_at[2];
        for (int i = 0; i < 6 && fp >= 0x40000000 && (fp & 7) == 0 && cl + 20 < sizeof chain; i++) {
            u64 *f = (u64 *)(usize)fp;
            cl += (usize)fmt(chain + cl, sizeof chain - cl, " +%llx", f[1] - b0);
            fp = f[0];
        }
        void *net_lock_owner(void);
        thr_t *no = net_lock_owner();
        thr_t *bo = bkl_owner_thr();
        klog("watchdog: the shell has not run its loop for %llu ms; state %d; last interrupted at +%llx (lr +%llx;%s); "
             "network lock: %s; big lock: %s", now - lp, m->state, arm_main_irq_at[0] - b0, arm_main_irq_at[1] - b0, chain,
             no ? thr_name(no) : "free", bo ? thr_name(bo) : "free");
        void thr_main_off(u64 *blocked, u64 *sleeping, u64 *waiting, u64 *site);
        u64 bl, sl, wt, site[8];
        thr_main_off(&bl, &sl, &wt, site);
        if (bl) klog("watchdog: the shell blocked %llu ms at +%llx +%llx +%llx +%llx", bl / 1000, site[0] - b0, site[1] - b0, site[2] - b0, site[3] - b0);
        char ln[200];
        usize l = 0;
        int idle = 0;
        for (int i = 0; i < n; i++) {                         /* every thread: name, state, system call */
            thr_t *t = all[i];
            if (t->state == T_BLOCKED && !t->in_sys && !strcmp(thr_name(t), "linux")) { idle++; continue; }   /* Linux's idle workers */
            char one[64];
            int k2 = fmt(one, sizeof one, " %s:%d", thr_name(t), t->state);
            if (t->in_sys) k2 += fmt(one + k2, sizeof one - (usize)k2, "/sys%u", t->sys_nr);
            if (l + (usize)k2 + 1 >= sizeof ln) { klog("watchdog:%s", ln); l = 0; }
            memcpy(ln + l, one, (usize)k2 + 1); l += (usize)k2;
        }
        if (l) klog("watchdog:%s (and %d idle Linux threads)", ln, idle);
    }
}

int hal_poll(event_t *out, int max) {
    u64 nowp = k_now_ms();
    static int wd;
    if (!wd++) thr_create("watchdog", watchdog, NULL, 16 << 10);
    void thr_longest(const char **name, u64 *us);
    const char *nm; u64 us;
    thr_longest(&nm, &us);                                    /* since the last poll */
    if (!(last_poll && nowp - last_poll > 250)) {
        void thr_main_off(u64 *blocked, u64 *sleeping, u64 *waiting, u64 *site);
        u64 a, b, c, d[8];
        thr_main_off(&a, &b, &c, d);                              /* counts start afresh each poll */
    }
    if (last_poll && nowp - last_poll > 250) {
        static int said;
        if (said++ < 20) {
            void thr_main_off(u64 *blocked, u64 *sleeping, u64 *waiting, u64 *site);
            u64 bl, sl, wt, site[8];
            thr_main_off(&bl, &sl, &wt, site);
            extern char _start[];
            u64 b0 = (u64)(usize)_start;
            char at[96] = "";                                     /* where it waited, for addr2line */
            if (bl) fmt(at, sizeof at, " at +%llx +%llx +%llx +%llx", site[0] - b0, site[1] - b0, site[2] - b0, site[3] - b0);
            klog("shell: stalled %llu ms (longest run without a switch: %s, %llu ms); off the CPU: blocked %llu ms%s, asleep %llu, waiting %llu",
                 nowp - last_poll, nm, us / 1000, bl / 1000, at, sl / 1000, wt / 1000);
        }
    }
    last_poll = nowp;
    static u64 report_at;
    if (k_now_ms() >= report_at) { thr_report(); report_at = k_now_ms() + 10000; }
    int n = virtio_input_poll(out, max);
    int linux_has_keys(void);
    if (!linux_has_keys()) n += msm_poll(out + n, max - n);  /* until Linux's gpio-keys has the key */
    n += linux_input_poll(out + n, max - n);
    /* volume up x3: the full-screen log (logview.c); while it is up, input goes nowhere */
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].type == EV_KEY && out[i].scan == SCAN_VOLUP && logview_key()) { if (!logview_active()) shell_redraw(); continue; }
        if (logview_active() && out[i].type == EV_KEY && out[i].scan == SCAN_VOLDN) { void logview_older(void); logview_older(); continue; }
        if (!logview_active()) out[m++] = out[i];
    }
    n = m;
    if (logview_expire()) shell_redraw();
    if (logview_active()) logview_draw();
    n += serial_keys(out + n, max - n);
    for (int i = 0; i < n; i++) if (out[i].type == EV_DOWN || out[i].type == EV_MOVE) { cur_x = out[i].x; cur_y = out[i].y; }
    return n;
}

/* ---- time ---- */
/* waiting lets the other threads (Linux) run */
void hal_delay_us(u32 us) { u64 end = k_now_us() + us; while (k_now_us() < end) __asm__ volatile("yield"); }
void hal_wait_frame_ms(u32 ms) { thr_sleep_us((u64)ms * 1000); }
void hal_wait_frame(void) {
    static u64 next;
    u64 now = k_now_us();
    /* late: straight on - a yield sent the shell behind every ready thread, a whole round
     * of slices (with Ladybird's dozens of threads, most of a second); the tick gives the
     * others their turns (sched.c) */
    if (next > now && next - now <= 10000) thr_sleep_until(next);
    next = MAX(now, next) + 10000;
}

/* PL031: seconds since 1970 (QEMU keeps it at the host's local time with -rtc base=localtime) */
static void civil(i64 s, EFI_TIME *t) {
    i64 days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    t->Hour = (u8)(rem / 3600); t->Minute = (u8)(rem % 3600 / 60); t->Second = (u8)(rem % 60);
    i64 z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    t->Day = (u8)(doy - (153 * mp + 2) / 5 + 1);
    t->Month = (u8)(mp < 10 ? mp + 3 : mp - 9);
    t->Year = (u16)(y + (t->Month <= 2));
}
int arm_rtc_get(EFI_TIME *t) {
    if (!pl031) return 0;
    memset(t, 0, sizeof *t);
    civil((i64)R32(pl031), t);
    return 1;
}
int arm_rtc_set(const EFI_TIME *t) { (void)t; return 0; }

/* ---- settings: in RAM, saved to the phone's storage by linux.c when it has some ---- */
#define NSET 64
static struct { c16 name[32]; u8 data[256]; usize len; int used; } sets[NSET];
static int set_find(const c16 *name, int create) {
    for (int i = 0; i < NSET; i++) {
        if (!sets[i].used) continue;
        int j = 0;
        while (j < 31 && name[j] && sets[i].name[j] == name[j]) j++;
        if (sets[i].name[j] == name[j]) return i;
    }
    if (!create) return -1;
    for (int i = 0; i < NSET; i++)
        if (!sets[i].used) {
            int j = 0;
            for (; j < 31 && name[j]; j++) sets[i].name[j] = name[j];
            sets[i].name[j] = 0;
            sets[i].used = 1;
            return i;
        }
    return -1;
}
u32 hal_setting_get(const c16 *name, u32 def) {
    int i = set_find(name, 0);
    u32 v = def;
    if (i >= 0 && sets[i].len == 4) memcpy(&v, sets[i].data, 4);
    return v;
}
void hal_setting_set(const c16 *name, u32 value) { hal_setting_set_blob(name, &value, 4); }
usize hal_setting_get_blob(const c16 *name, void *buf, usize cap) {
    int i = set_find(name, 0);
    if (i < 0) return 0;
    usize n = MIN(cap, sets[i].len);
    memcpy(buf, sets[i].data, n);
    return n;
}
static u32 set_gen;                                   /* changes since boot: linux.c saves them */
static int set_ready;
void hal_setting_set_blob(const c16 *name, const void *buf, usize len) {
    int i = set_find(name, 1);
    if (i < 0) return;
    usize n = MIN(len, sizeof sets[i].data);
    if (sets[i].len == n && !memcmp(sets[i].data, buf, n)) return;
    sets[i].len = n;
    memcpy(sets[i].data, buf, n);
    set_gen++;
}
/* kept on the phone (linux.c: a file on the logdump partition) - the format: per setting
 * a name (32 UTF-16 units), a length (u32) and the bytes */
u32 hal_settings_gen(void) { return set_gen; }
usize hal_settings_export(u8 *buf, usize cap) {
    usize o = 0;
    for (int i = 0; i < NSET; i++) {
        if (!sets[i].used) continue;
        usize need = sizeof sets[i].name + 4 + sets[i].len;
        if (o + need > cap) break;
        memcpy(buf + o, sets[i].name, sizeof sets[i].name); o += sizeof sets[i].name;
        u32 l = (u32)sets[i].len;
        memcpy(buf + o, &l, 4); o += 4;
        memcpy(buf + o, sets[i].data, l); o += l;
    }
    return o;
}
/* what was saved, under what was set since boot */
int hal_settings_import(const u8 *buf, usize len) {
    int n = 0;
    for (usize o = 0; o + sizeof sets[0].name + 4 <= len;) {
        c16 name[32];
        memcpy(name, buf + o, sizeof name); o += sizeof name;
        name[31] = 0;
        u32 l;
        memcpy(&l, buf + o, 4); o += 4;
        if (l > sizeof sets[0].data || o + l > len) break;
        if (set_find(name, 0) < 0) {
            int i = set_find(name, 1);
            if (i >= 0) { sets[i].len = l; memcpy(sets[i].data, buf + o, l); n++; }
        }
        o += l;
    }
    return n;
}
void hal_settings_mark_ready(void) { set_ready = 1; }
int hal_settings_ready(void) { return set_ready || k_now_ms() > 90000; }
void hal_settings_prepare(void) {}
void hal_probe(void) {}

void *hal_dma_alloc(usize bytes) { return (void *)(usize)pmm_alloc_contig((bytes + 4095) / 4096); }
const char *hal_mode(void) { return "native kernel (arm64)"; }
int hal_form_phone(void) { return 1; }
int hal_fb_direct(void) { return 0; }                   /* fb.c converts (24-bit panels) and tracks what changed */                  /* the ARM targets are phones (QEMU stands in for one) */

/* ---- power ---- */
static void psci(u64 fn) {
    if (psci_hvc < 0) return;
    register u64 x0 __asm__("x0") = fn;
    if (psci_hvc) __asm__ volatile("hvc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
    else __asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
}
void power_off(void) { klog("power: off"); plog_state(PLOG_CLEAN); psci(0x84000008); for (;;) __asm__ volatile("wfe"); }
void power_restart(void) { klog("power: restart"); plog_state(PLOG_CLEAN); psci(0x84000009); for (;;) __asm__ volatile("wfe"); }
void hal_shutdown(void) { power_off(); }
void hal_reboot(void) { power_restart(); }
int  hal_reboot_to_firmware(void) { return 0; }
