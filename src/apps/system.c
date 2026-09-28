/* System: what Tessera found on this machine, plus the kernel log. */
#include "../ui/shell.h"
#include "../kernel/smp.h"

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

    heading(&f, "FIRMWARE (THE DRIVER LAYER)");
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
    kv(&f, "Keys", "Console input (keyboard, hardware buttons)");

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

    heading(&f, "KERNEL");
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
    return 1;   /* uptime */
}

const app_t app_system = { "System", "Hardware & drivers", RGB(0x7c, 0x6c, 0xff), icon, NULL, draw, event, tick };
