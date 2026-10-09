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

int hal_poll(event_t *out, int max) {
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
    if (next > now && next - now <= 10000) thr_sleep_until(next); else thr_yield();
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

/* ---- settings: in RAM for now (the phone's storage comes with Linux's drivers) ---- */
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
void hal_setting_set_blob(const c16 *name, const void *buf, usize len) {
    int i = set_find(name, 1);
    if (i < 0) return;
    sets[i].len = MIN(len, sizeof sets[i].data);
    memcpy(sets[i].data, buf, sets[i].len);
}
void hal_settings_prepare(void) {}
void hal_probe(void) {}

void *hal_dma_alloc(usize bytes) { return (void *)(usize)pmm_alloc_contig((bytes + 4095) / 4096); }
const char *hal_mode(void) { return "native kernel (arm64)"; }

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
