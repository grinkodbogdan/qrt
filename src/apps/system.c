/* System: what Tessera found on this machine, plus the kernel log. */
#include "../ui/shell.h"
#include "../kernel/smp.h"
#include "../kernel/vfs.h"
#include "../kernel/dev.h"
#if defined(__x86_64__)
#include "../arch/x64/sched.h"
#include "../arch/x64/irq.h"
#endif

/* sampled on the boot core in tick(); draw() may run on any core */
static struct { int busy_pct, threads; u64 mem_total, mem_free; char thread_list[160], irqs[128]; } ks;

static struct { scroll_t sc; } st;

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

static void heading(flow_t *f, const char *t) {
    f->y += dp(14);
    ui_section(f->c, f->col.x, f->y, t);
    f->y += ui.small->line + dp(4);
}

static void kv(flow_t *f, const char *key, const char *val) {
    rect_t r = { f->col.x, f->y, f->col.w, ui.body->line };
    if (f->y + r.h > f->c->clip.y && f->y < f->c->clip.y + f->c->clip.h) ui_kv(f->c, r, key, val);
    f->y += ui.body->line + dp(2);
}

static void draw(canvas_t *c, rect_t a) {
    rect_t card = { a.x + dp(16), a.y, a.w - dp(32), a.h - dp(8) };
    ui_card(c, card, dp(22), 0);
    rect_t inner = { card.x + dp(22), card.y + dp(10), card.w - dp(44), card.h - dp(20) };
    gfx_clip(c, inner);
    flow_t f = { c, inner, inner.y - st.sc.off };
    char b[128], b2[32];

    if (k.is_venue) {
        f.y += dp(10);
        gfx_rrect(c, (rect_t){ inner.x, f.y, inner.w, dp(44) }, dp(12), ALPHA(ui.accent, 60));
        gfx_text(c, ui.label, inner.x + dp(14), f.y + (dp(44) - ui.label->line) / 2, "Dell Venue 8 Pro detected", ui.text);
        f.y += dp(48);
    }
    heading(&f, "GRAPHICS");
    fmt(b, sizeof b, "%u.%u ms draw + %u.%u ms present, %u%% of screen",
        shell_stats.compose_us / 1000, shell_stats.compose_us / 100 % 10,
        shell_stats.present_us / 1000, shell_stats.present_us / 100 % 10, shell_stats.area_permille / 10);
    kv(&f, "Last frame", b);
    if (shell_stats.bench[0][0]) {
        kv(&f, "Benchmark", shell_stats.bench[0]);
        kv(&f, "", shell_stats.bench[1]);
        kv(&f, "", shell_stats.bench[2]);
        kv(&f, "", shell_stats.bench[3]);
    } else kv(&f, "Benchmark", "type \"bench\" in the Ask bar");

    heading(&f, "KERNEL");
    kv(&f, "Mode", k.native ? "native: firmware exited, QRT owns the machine" : "firmware-hosted (UEFI boot services running)");
    if (k.native) {
        fmt(b, sizeof b, "%d threads, CPU %d%% busy", ks.threads, ks.busy_pct);
        kv(&f, "Scheduler", b);
        kv(&f, "Threads", ks.thread_list);
        char t1[24], t2[24];
        fmt_bytes(t1, sizeof t1, ks.mem_total);
        fmt_bytes(t2, sizeof t2, ks.mem_free);
        fmt(b, sizeof b, "%s managed, %s free (own page allocator + heap)", t1, t2);
        kv(&f, "Memory", b);
        fmt(b, sizeof b, "%d (boot core + %d started by QRT)", smp_workers() + 1, smp_workers());
        kv(&f, "CPU cores", b);
        kv(&f, "Timer", "local APIC, 1000 Hz; the CPU halts when idle");
    }

    if (k.native) kv(&f, "Interrupts", ks.irqs);

    fmt(b, sizeof b, "DEVICES (%d, %d WITH A QRT DRIVER)", n_devs, dev_bound());
    heading(&f, b);
    if (!pci_ndevs) kv(&f, "PCI", "no ECAM (MCFG) table");
    /* bound devices first, then the to-do list */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n_devs; i++) {
            device_t *d = &devs[i];
            int bound = d->drv && !d->failed;
            if (bound != !pass) continue;
            char key[32];
            fmt(key, sizeof key, "%s %s", d->bus == BUS_PCI ? "pci" : d->bus == BUS_ACPI ? "acpi" : "isa", d->name);
            if (bound) fmt(b, sizeof b, "%s: %s", d->drv->name, d->status);
            else if (d->failed) fmt(b, sizeof b, "%s failed: %s", d->drv->name, d->status);
            else if (d->pci) fmt(b, sizeof b, "%s %04x:%04x - no driver yet", d->what, d->pci->vendor, d->pci->device);
            else fmt(b, sizeof b, "%s - no driver yet", d->what ? d->what : "unknown device");
            kv(&f, key, b);
        }
    if (k.native) kv(&f, "Files", "RAM copy of the boot stick (vfs); native storage pending");

    heading(&f, "DEVICE");
    kv(&f, "Manufacturer", k.sys_vendor[0] ? k.sys_vendor : "unknown");
    kv(&f, "Model", k.sys_product[0] ? k.sys_product : "unknown");
    kv(&f, "BIOS", k.bios_version[0] ? k.bios_version : "unknown");
    kv(&f, "ACPI OEM", k.acpi_oem[0] ? k.acpi_oem : "none");

    heading(&f, "PROCESSOR & MEMORY");
    kv(&f, "CPU", k.cpu);
    fmt(b, sizeof b, "%d worker cores for drawing%s", smp_workers(), smp_enabled() ? "" : " (multicore off)");
    kv(&f, "Cores", b);
    fmt_bytes(b2, sizeof b2, k.ram_bytes);
    kv(&f, "RAM", b2);
    fmt(b, sizeof b, "%llu MHz (TSC)", k.tsc_per_ms / 1000);
    kv(&f, "Clock", b);

    heading(&f, k.native ? "FIRMWARE (BOOT ONLY; RUNTIME: CLOCK, SETTINGS, RESET)" : "FIRMWARE (THE DRIVER LAYER)");
    kv(&f, "Vendor", k.fw_vendor);
    fmt(b, sizeof b, "UEFI %u.%u%s, %s", k.uefi_revision >> 16, (k.uefi_revision & 0xffff) / 10,
        (k.uefi_revision & 0xffff) % 10 ? "x" : "", sizeof(void *) == 8 ? "64-bit" : "32-bit");
    kv(&f, "Interface", b);
    fmt(b, sizeof b, "%d handles, %d controllers bound", k.handles, k.drivers_connected);
    kv(&f, "Drivers", b);

    heading(&f, "DISPLAY & INPUT");
    fmt(b, sizeof b, "%u \xc3\x97 %u via GOP, rotation %d\xc2\xb0", k.fb_w, k.fb_h, shell_rotation() * 90);
    kv(&f, "Panel", b);
    int s100 = (int)(ui.s * 100 + 0.5f);
    fmt(b, sizeof b, "%d.%02dx  \xc2\xb7  %d \xc3\x97 %d dp", s100 / 100, s100 % 100, (int)(ui.W / ui.s), (int)(ui.H / ui.s));
    kv(&f, "UI scale", b);
    fmt(b, sizeof b, "%d touch, %d pointer", k.n_abs, k.n_rel);
    kv(&f, "Pointers", b);
    kv(&f, "Keys", k.native ? "serial console (USB keyboard driver pending)" : "Console input (keyboard, hardware buttons)");

    heading(&f, "STORAGE");
    for (int i = 0; i < k.n_blk; i++) {
        if (k.blk[i].partition) continue;
        fmt_bytes(b2, sizeof b2, k.blk[i].bytes);
        fmt(b, sizeof b, "%s%s%s", b2, k.blk[i].removable ? ", removable" : ", built-in", k.blk[i].read_only ? ", read-only" : "");
        kv(&f, "Disk", b);
    }
    for (int i = 0; i < k.n_vol; i++) {
        fmt_bytes(b2, sizeof b2, k.vol[i].size);
        fmt(b, sizeof b, "%s  %s%s", k.vol[i].label, b2, k.vol[i].boot ? "  (boot)" : "");
        kv(&f, "Volume", b);
    }

    heading(&f, "BUILD");
    kv(&f, "System", "QRT " QRT_VERSION " / Tessera");
    kv(&f, "Architecture", QRT_ARCH);
    u64 up = k_now_ms() / 1000;
    fmt(b, sizeof b, "%lluh %02llum %02llus", up / 3600, up / 60 % 60, up % 60);
    kv(&f, "Uptime", b);
    fmt(b, sizeof b, "%llu ms to first frame", k.boot_ms);
    kv(&f, "Boot", b);

    heading(&f, "BOOT LOG");
    for (int i = 0; klog_line(i); i++) {
        if (f.y + ui.small->line > inner.y && f.y < inner.y + inner.h)
            gfx_text_fit(c, ui.small, inner.x, f.y, inner.w, klog_line(i), ui.text2);
        f.y += ui.small->line;
    }
    f.y += dp(16);
    st.sc.max = f.y + st.sc.off - inner.y - inner.h;
    gfx_unclip(c);
    gfx_clip(c, a);
}

static int event(const event_t *e, rect_t a) { return scroll_event(&st.sc, e, a, dp(48)); }


static int tick(u64 now) {
    static u64 last;
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
        for (int i = 0; i < ks.threads; i++)
            o += fmt(ks.thread_list + o, sizeof ks.thread_list - o, "%s%s", i ? ", " : "", th[i]->name);
        ks.mem_total = pmm_total_bytes();
        ks.mem_free = pmm_free_bytes();
        const irq_line_t *l;
        int n = irq_lines(&l);
        o = fmt(ks.irqs, sizeof ks.irqs, "%d I/O APIC%s", irq_ioapics(), irq_ioapics() == 1 ? "" : "s");
        for (int i = 0; i < n; i++)
            o += fmt(ks.irqs + o, sizeof ks.irqs - o, "; %s vector 0x%x: %llu", l[i].owner, l[i].vector, l[i].count);
    }
#endif
    dev_refresh();
    return 1;   /* uptime */
}

const app_t app_system = { "System", "Hardware & drivers", RGB(0x7c, 0x6c, 0xff), icon, NULL, draw, event, tick };
