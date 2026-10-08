/* System Monitor: resources (CPU, memory, network, tasks), hardware, and the log. */
#include "../ui/shell.h"
#include "../kernel/sound.h"
#include "../drivers/i915/gpu.h"
#include "../drivers/i915/display.h"
#include "../drivers/usb/xhci.h"
#include "../drivers/audio.h"
#include "../drivers/hda.h"
#if defined(__x86_64__)
#include "../acpi/acpidev.h"
#endif
#include "../kernel/smp.h"
#include "../kernel/vfs.h"
#include "../kernel/dev.h"
#include "../net/net.h"
#include "../drivers/buttons.h"
#if defined(__x86_64__)
#include "../arch/x64/sched.h"
#include "../arch/x64/irq.h"
#endif

/* sampled on the boot core in tick(); draw() may run on any core */
static struct {
    int busy_pct, threads; u64 mem_total, mem_free; char thread_list[160], irqs[128];
    u8 cpu_hist[60]; int n_hist;                 /* CPU busy %, one sample a second */
    char names[16][24];
} ks;

static struct { scroll_t sc; tap_t tap; int tab; } st;
enum { TAB_RES, TAB_HW, TAB_LOG, TAB_BTN };   /* TAB_BTN: the Button test, from the launcher */
static const char *tabs[] = { "Resources", "Hardware", "Log" };

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    rect_t chip = { (int)(cx - r * 0.6f), (int)(cy - r * 0.6f), (int)(r * 1.2f), (int)(r * 1.2f) };
    gfx_rrect_outline(c, chip, (int)(r * 0.2f), (int)(r * 0.2f) + 1, fg);
    for (int i = -1; i <= 1; i++) {
        float o = i * r * 0.38f;
        gfx_line(c, cx + o, cy - r, cx + o, cy - r * 0.6f, r * 0.14f, fg);
        gfx_line(c, cx + o, cy + r * 0.6f, cx + o, cy + r, r * 0.14f, fg);
        gfx_line(c, cx - r, cy + o, cx - r * 0.6f, cy + o, r * 0.14f, fg);
        gfx_line(c, cx + r * 0.6f, cy + o, cx + r, cy + o, r * 0.14f, fg);
    }
    gfx_circle(c, cx, cy, r * 0.2f, fg);
}

typedef struct { canvas_t *c; rect_t col; int y; } flow_t;

static rect_t tab_rect(rect_t a, int i) {
    int w = dp(120), x0 = a.x + (a.w - 3 * w) / 2;
    return (rect_t){ x0 + i * w, a.y + dp(12), w, dp(36) };
}
static rect_t body_rect(rect_t a) {
    int w = MIN(a.w - dp(48), dp(760)), y = a.y + dp(60);
    return (rect_t){ a.x + (a.w - w) / 2, y, w, a.y + a.h - y };
}

#define ROW dp(44)
static void heading(flow_t *f, const char *t) {
    f->y += dp(18);
    gfx_text(f->c, ui.label, f->col.x + dp(4), f->y, t, ui.text);
    f->y += ui.label->line + dp(8);
}
/* a row inside a rounded group: key on the left, value on the right */
static void kv(flow_t *f, const char *key, const char *val) {
    rect_t r = { f->col.x, f->y, f->col.w, ROW };
    if (r.y + r.h > f->c->clip.y && r.y < f->c->clip.y + f->c->clip.h) {
        gfx_fill(f->c, r, RGBA(255, 255, 255, 12));
        gfx_fill(f->c, (rect_t){ r.x, r.y + r.h - 1, r.w, 1 }, RGBA(0, 0, 0, 70));
        int ty = r.y + (ROW - ui.body->line) / 2;
        gfx_text_fit(f->c, ui.body, r.x + dp(16), ty, r.w * 2 / 5, key, ui.text);
        int vw = MIN(text_width(ui.body, val), r.w * 3 / 5 - dp(24));
        gfx_text_fit(f->c, ui.body, r.x + r.w - dp(16) - vw, ty, r.w * 3 / 5 - dp(24), val, ui.text2);
    }
    f->y += ROW;
}

static void meter(flow_t *f, const char *label, const char *val, int pct) {
    rect_t r = { f->col.x, f->y, f->col.w, dp(64) };
    gfx_rrect(f->c, r, dp(12), RGBA(255, 255, 255, 12));
    gfx_text(f->c, ui.body, r.x + dp(16), r.y + dp(10), label, ui.text);
    int vw = text_width(ui.body, val);
    gfx_text(f->c, ui.body, r.x + r.w - dp(16) - vw, r.y + dp(10), val, ui.text2);
    rect_t bar = { r.x + dp(16), r.y + dp(42), r.w - dp(32), dp(8) };
    gfx_rrect(f->c, bar, dp(4), RGBA(255, 255, 255, 30));
    if (pct > 0) gfx_rrect(f->c, (rect_t){ bar.x, bar.y, MAX(dp(8), bar.w * pct / 100), bar.h }, dp(4), ui.accent);
    f->y += r.h + dp(10);
}

/* the CPU graph: the last minute, one bar a second */
static void cpu_graph(flow_t *f) {
    rect_t r = { f->col.x, f->y, f->col.w, dp(150) };
    gfx_rrect(f->c, r, dp(12), RGBA(255, 255, 255, 12));
    char v[32];
    fmt(v, sizeof v, "%d%%", ks.busy_pct);
    gfx_text(f->c, ui.body, r.x + dp(16), r.y + dp(10), "Processor", ui.text);
    gfx_text(f->c, ui.body, r.x + r.w - dp(16) - text_width(ui.body, v), r.y + dp(10), v, ui.text2);
    rect_t g = { r.x + dp(16), r.y + dp(42), r.w - dp(32), r.h - dp(56) };
    for (int i = 1; i < 4; i++) gfx_fill(f->c, (rect_t){ g.x, g.y + g.h * i / 4, g.w, 1 }, RGBA(255, 255, 255, 18));
    float bw = (float)g.w / 60;
    for (int i = 0; i < ks.n_hist; i++) {
        int h = MAX(1, g.h * ks.cpu_hist[i] / 100);
        int x = g.x + (int)((60 - ks.n_hist + i) * bw);
        gfx_fill(f->c, (rect_t){ x, g.y + g.h - h, MAX(1, (int)bw - 1), h }, ALPHA(ui.accent, 200));
    }
    f->y += r.h + dp(10);
}

static const char *friendly(const device_t *d) {
    const char *n = d->drv ? d->drv->name : "";
    if (!strcmp(n, "iwm (Wi-Fi)")) return "Wi-Fi adapter";
    if (!strcmp(n, "gpio-buttons")) return "Buttons";
    if (!strcmp(n, "backlight")) return "Backlight";
    if (!strcmp(n, "i2c-hid")) return "Touchscreen";
    if (!strcmp(n, "framebuffer")) return "Display";
    if (!strcmp(n, "i915")) return "Graphics";
    if (!strcmp(n, "xhci")) return "USB";
    if (!strcmp(n, "audio")) return "Sound";
    if (!strcmp(n, "e1000")) return "Ethernet";
    if (!strcmp(n, "uart16550")) return "Serial port";
    if (!strcmp(n, "dw-i2c") || !strcmp(n, "designware-i2c")) return "I2C bus";
    return n;
}

static void draw(canvas_t *c, rect_t a) {
    for (int i = 0; i < 3; i++) {
        rect_t t = tab_rect(a, i);
        gfx_rrect(c, t, dp(8), i == st.tab ? RGBA(255, 255, 255, 36) : RGBA(255, 255, 255, 0));
        gfx_text_center(c, i == st.tab ? ui.label : ui.body, t, tabs[i], i == st.tab ? ui.text : ui.text2);
    }
    rect_t body = body_rect(a);
    rect_t old = c->clip;
    gfx_clip(c, body);
    flow_t f = { c, body, body.y - st.sc.off };
    char b[160], b2[32];

    if (st.tab == TAB_RES) {
        f.y += dp(4);
        cpu_graph(&f);
        u64 total = k.native ? ks.mem_total : k.ram_bytes, used = k.native ? ks.mem_total - ks.mem_free : 0;
        char t1[24], t2[24];
        fmt_bytes(t1, sizeof t1, used);
        fmt_bytes(t2, sizeof t2, total);
        fmt(b, sizeof b, "%s of %s", t1, t2);
        meter(&f, "Memory", k.native ? b : t2, total ? (int)(used * 100 / total) : 0);
        heading(&f, "Network");
        const char *ns = net_status();
        kv(&f, "Connection", ns ? ns : "Offline");
        heading(&f, "System");
        u64 up = k_now_ms() / 1000;
        fmt(b, sizeof b, "%llu:%02llu:%02llu", up / 3600, up / 60 % 60, up % 60);
        kv(&f, "Uptime", b);
        fmt(b, sizeof b, "%d", smp_workers() + 1);
        kv(&f, "Processor cores", b);
        if (k.native) {
            heading(&f, "Tasks");
            for (int i = 0; i < ks.threads && i < 16; i++) kv(&f, ks.names[i], "running");
        }
    } else if (st.tab == TAB_HW) {
        f.y += dp(4);
        heading(&f, "Device");
        kv(&f, "Model", k.sys_product[0] ? k.sys_product : "Unknown");
        kv(&f, "Manufacturer", k.sys_vendor[0] ? k.sys_vendor : "Unknown");
        kv(&f, "Processor", k.cpu);
        fmt_bytes(b2, sizeof b2, k.ram_bytes);
        kv(&f, "Memory", b2);
        fmt(b, sizeof b, "%u \xc3\x97 %u", k.fb_w, k.fb_h);
        kv(&f, "Display", b);
        kv(&f, "Firmware", k.bios_version[0] ? k.bios_version : k.fw_vendor);
#if defined(__x86_64__)
        kv(&f, "ACPI", acpi_status());
        { int linuxdrv_mounts(char *out, int cap); char m[256]; if (linuxdrv_mounts(m, sizeof m)) kv(&f, "Disks (Linux)", m); }
#endif
        for (int i = 0; i < k.n_vol; i++) {
            fmt_bytes(b2, sizeof b2, k.vol[i].size);
            fmt(b, sizeof b, "%s, %s", k.vol[i].label[0] ? k.vol[i].label : "Volume", b2);
            kv(&f, k.vol[i].boot ? "Boot drive" : "Drive", b);
        }
        heading(&f, "Sound");
        kv(&f, "Output", snd_status());
        kv(&f, "Built-in codec", k.is_venue ? audio_status() : hda_status());
        heading(&f, "Graphics");
        kv(&f, "Acceleration", gpu_status());
        kv(&f, "External display", display_status());
        if (gpu_active()) {
            u32 frames, avg;
            int coherent;
            gpu_stats(&frames, &avg, &coherent);
            fmt(b, sizeof b, "%u frames, %u \xc2\xb5s each on average", frames, avg);
            kv(&f, "GPU work", b);
        }
        {
            char ul[8][96];
            int nu = xhci_devices(ul, 8);
            if (nu) {
                heading(&f, "USB");
                for (int i = 0; i < nu; i++) {
                    char key[16];
                    fmt(key, sizeof key, "Device %d", i + 1);
                    kv(&f, key, ul[i]);
                }
            }
        }
        heading(&f, "Drivers");
        for (int i = 0; i < n_devs; i++) {
            device_t *d = &devs[i];
            if (!d->drv || d->failed || !strcmp(d->drv->name, "chipset")) continue;
            kv(&f, friendly(d), d->status);
        }
        if (shell_stats.bench[0][0]) {
            heading(&f, "Graphics benchmark");
            for (int i = 0; i < 4; i++) kv(&f, i == 0 ? "Full screen" : i == 1 ? "Small area" : i == 2 ? "Copy" : "Cores", shell_stats.bench[i]);
        }
        heading(&f, "Software");
        kv(&f, "Operating system", "QRT " QRT_VERSION);
        kv(&f, "Kernel", k.native ? "Tessera (native)" : "Tessera (on the firmware)");
        kv(&f, "Architecture", QRT_ARCH);
    } else if (st.tab == TAB_BTN) {
        f.y += dp(4);
        heading(&f, "Button test");
        gfx_text_fit(c, ui.body, body.x + dp(4), f.y, body.w, "Press each button: its line should change. A photo of this screen helps find a fault.", ui.text2);
        f.y += ui.body->line + dp(12);
        char lines[10][112];
        int n = buttons_debug(lines, 10);
        const font_t *m = font_pick(F_MONO, dp(12));
        rect_t box = { body.x, f.y, body.w, n * (m->line + dp(6)) + dp(20) };
        gfx_rrect(c, box, dp(12), RGB(0x1d, 0x1d, 0x20));
        for (int i = 0; i < n; i++) gfx_text_fit(c, m, box.x + dp(12), box.y + dp(10) + i * (m->line + dp(6)), box.w - dp(24), lines[i], RGB(0xde, 0xdd, 0xda));
        f.y = box.y + box.h;
    } else {
        const font_t *m = font_pick(F_MONO, dp(12));
        rect_t box = { body.x, f.y + dp(4), body.w, 0 };
        /* long lines wrap (the font is monospaced), so the end of an error is not cut off */
        int cols = MAX(20, (box.w - dp(24)) / MAX(1, text_width(m, "M")));
        int n = 0, rows = 0;
        while (klog_line(n)) rows += MAX(1, ((int)strlen(klog_line(n)) + cols - 1) / cols), n++;
        box.h = rows * m->line + dp(20);
        gfx_rrect(c, box, dp(12), RGB(0x1d, 0x1d, 0x20));
        int y = box.y + dp(10);
        for (int i = 0; i < n; i++) {
            const char *l = klog_line(i);
            int len = (int)strlen(l);
            for (int o = 0; o == 0 || o < len; o += cols, y += m->line) {
                if (y + m->line <= body.y || y >= body.y + body.h) continue;
                char seg[256];
                strlcpy(seg, l + o, (usize)MIN(cols + 1, (int)sizeof seg));
                gfx_text(c, m, box.x + dp(12), y, seg, RGB(0xde, 0xdd, 0xda));
            }
        }
        f.y = box.y + box.h;
    }
    f.y += dp(24);
    st.sc.max = MAX(0, f.y + st.sc.off - body.y - body.h);
    c->clip = old;
}

static int event(const event_t *e, rect_t a) {
    if (e->type == EV_KEY && e->scan == 0x7f01) { st.tab = TAB_HW; st.sc.off = 0; return 1; }   /* from the shell after "bench" */
    if (e->type == EV_KEY && e->scan == 0x7f02) { st.tab = TAB_BTN; st.sc.off = 0; buttons_scan_start(); return 1; }  /* "Button test" */
    if (tap_track(&st.tap, e, dp(12)))
        for (int i = 0; i < 3; i++) if (in_rect(tab_rect(a, i), e->x, e->y)) { st.tab = i; st.sc.off = 0; return 1; }
    return scroll_event(&st.sc, e, body_rect(a), dp(48));
}


static int tick(u64 now) {
    static u64 last;
    if (st.tab == TAB_BTN && now - last >= 100) { last = now; return 1; }   /* live */
    if (now - last < 1000) return 0;
    last = now;
#if defined(__x86_64__)
    if (k.native) {
        static u64 last_idle, last_ticks;
        u64 idle = sched_idle_ticks(), t = ticks;
        if (t > last_ticks) ks.busy_pct = 100 - (int)((idle - last_idle) * 100 / (t - last_ticks));
        last_idle = idle; last_ticks = t;
        thread_t *th[16];
        ks.threads = sched_threads(th, 16);
        usize o = 0;
        ks.thread_list[0] = 0;
        for (int i = 0; i < ks.threads; i++) {
            o += fmt(ks.thread_list + o, sizeof ks.thread_list - o, "%s%s", i ? ", " : "", th[i]->name);
            if (i < 16) strlcpy(ks.names[i], th[i]->name, sizeof ks.names[i]);
        }
        ks.mem_total = pmm_total_bytes();
        ks.mem_free = pmm_free_bytes();
        const irq_line_t *l;
        int n = irq_lines(&l);
        o = fmt(ks.irqs, sizeof ks.irqs, "%d I/O APIC%s", irq_ioapics(), irq_ioapics() == 1 ? "" : "s");
        for (int i = 0; i < n; i++)
            o += fmt(ks.irqs + o, sizeof ks.irqs - o, "; %s vector 0x%x: %llu", l[i].owner, l[i].vector, l[i].count);
    }
#endif
    if (ks.n_hist == 60) { memmove(ks.cpu_hist, ks.cpu_hist + 1, 59); ks.n_hist--; }
    ks.cpu_hist[ks.n_hist++] = (u8)CLAMP(ks.busy_pct, 0, 100);
    dev_refresh();
    return 1;   /* uptime */
}

const app_t app_system = { "System Monitor", "Processor, memory and hardware", RGB(0x3a, 0x94, 0x4a), icon, NULL, draw, event, tick };
