/*
 * fb.c - the display on 64-bit ARM.
 *
 * QEMU (virt): ramfb - a framebuffer in RAM that QEMU scans out, set up with one
 * fw_cfg DMA write ("etc/ramfb").
 *
 * Qualcomm phones (MSM8953: Xiaomi Mi A1): the boot loader leaves the panel on
 * ("continuous splash") with an MDP5 source pipe scanning a buffer in RAM.  Its
 * registers say where, how big and in what byte order, so Tessera draws into that
 * buffer, converting its 32-bit pixels to the pipe's format (RGB888 as a rule).  Pipe
 * offsets from Linux's mdp5_cfg.c (msm8x53_config); MDP at 0x1a01000.
 */
#include "arm.h"
#include "sched.h"
#include "../../kernel/smp.h"

char fb_what[128] = "none";
u64 fb_other[4][2]; int fb_nother;                 /* other layers' memory: kept out of the heap (main.c) */
u64 fb_reserve_base, fb_reserve_size;      /* mapped uncached by mmu.c (as in 0.17.0) */
static u8 *fb;
static int bpp = 4, byte_of[4];            /* byte_of[c]: where channel c (0 R, 1 G, 2 B) goes */
static u64 kick;                           /* a command-mode panel's CTL START register */

/* ---- QEMU ramfb through fw_cfg ---- */
static u32 be32(u32 v) { return __builtin_bswap32(v); }
static int ramfb_init(void) {
    int n = fdt_find_compatible(-1, "qemu,fw-cfg-mmio");
    u64 cfg, sz;
    if (n < 0 || !fdt_reg(n, 0, &cfg, &sz)) return 0;
    /* the file directory: select 0x19, read big-endian count, then 64-byte entries */
    *(volatile u16 *)(usize)(cfg + 8) = __builtin_bswap16(0x19);
    u8 b[64];
    for (int i = 0; i < 4; i++) b[i] = *(volatile u8 *)(usize)cfg;
    u32 count = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3], sel = 0;
    for (u32 f = 0; f < count && !sel; f++) {
        for (int i = 0; i < 64; i++) b[i] = *(volatile u8 *)(usize)cfg;
        if (!strcmp((char *)b + 8, "etc/ramfb")) sel = (u32)b[4] << 8 | b[5];
    }
    if (!sel) return 0;
    /* the size: chosen bootargs "qrt.fb=WxH", else a phone-like 720 x 1280 */
    u32 w = 720, h = 1280;
    int ch = fdt_node("/chosen"), len;
    const char *args = ch >= 0 ? fdt_prop(ch, "bootargs", &len) : NULL;
    const char *p = args ? strstr(args, "qrt.fb=") : NULL;
    if (p) {
        u32 a = 0, c = 0;
        for (p += 7; *p >= '0' && *p <= '9'; p++) a = a * 10 + (u32)(*p - '0');
        if (*p == 'x') for (p++; *p >= '0' && *p <= '9'; p++) c = c * 10 + (u32)(*p - '0');
        if (a >= 320 && c >= 320 && a <= 4096 && c <= 4096) { w = a; h = c; }
    }
    /* the buffer: near the top of the first RAM bank (kept out of the heap as a hole) */
    int mem = fdt_node("/memory");
    u64 rb, rs;
    if (mem < 0 || !fdt_reg(mem, 0, &rb, &rs)) return 0;
    int b3 = args && strstr(args, "qrt.fb24");          /* 3-byte pixels, as the Mi A1's panel: for testing */
    u64 bytes = ((u64)w * h * (b3 ? 3 : 4) + 0x1fffff) & ~0x1fffffull;
    u64 base = (rb + rs - (16ull << 20) - bytes) & ~0x1fffffull;   /* clear of anything kept at the very top */
    static struct __attribute__((packed)) { u64 addr; u32 fourcc, flags, width, height, stride; } rc;
    rc.addr = __builtin_bswap64(base);
    rc.fourcc = be32(b3 ? 0x34324752 : 0x34325258);     /* RG24: RGB888, XR24: XRGB8888 */
    rc.flags = 0;
    rc.width = be32(w); rc.height = be32(h); rc.stride = be32(w * (b3 ? 3 : 4));
    static volatile struct __attribute__((packed)) { u32 control, length; u64 address; } dma;
    dma.control = be32(sel << 16 | 0x08 | 0x10);        /* select, write */
    dma.length = be32(sizeof rc);
    dma.address = __builtin_bswap64((u64)(usize)&rc);
    u64 da = (u64)(usize)&dma;
    W32(cfg + 16, be32((u32)(da >> 32)));
    W32(cfg + 20, be32((u32)da));                      /* writing the low half starts it */
    for (int i = 0; i < 1000000 && (be32(dma.control) & ~1u); i++) {}
    fb = (u8 *)(usize)base;
    fb_reserve_base = base; fb_reserve_size = bytes;
    k.fb_w = w; k.fb_h = h; k.fb_stride = w; k.fb_base = base;
    bpp = b3 ? 3 : 4; byte_of[0] = 2; byte_of[1] = 1; byte_of[2] = 0;
    fmt(fb_what, sizeof fb_what, b3 ? "QEMU ramfb (RGB888)" : "QEMU ramfb");
    return 1;
}

/* ---- Qualcomm MDP5: the pipe the boot loader's splash screen scans out ---- */
/* where each source pipe sits in a CTL's LAYER / LAYER_EXT registers and its FLUSH bit
 * (MSM8953's MDP 1.16: VIG0, RGB0, RGB1, DMA0, cursor) */
static const struct { u32 off; int sh, ext, flush; } mdp_pipes[] = {
    { 0x04000, 0, 0, 0 }, { 0x14000, 9, 8, 3 }, { 0x16000, 12, 10, 4 }, { 0x24000, 18, 16, 11 }, { 0x34000, -1, 20, 22 },
};
static int pipe_stage(u32 lay, u32 ext, int j) {
    if (mdp_pipes[j].sh < 0) return (int)(ext >> 20 & 0xf);                         /* the cursor: 4 bits in EXT */
    return (int)(lay >> mdp_pipes[j].sh & 7) | (int)(ext >> mdp_pipes[j].ext & 1) << 3;
}
/* the boot loader may leave more than one layer on (a logo, a warning over the splash).
 * Their memory stays out of Tessera's heap (main.c), so they keep showing what the boot
 * loader drew - as in 0.17.0.  Tessera never writes the MDP's layer registers: switching
 * layers off by hand (0.19.x) was one of the changes the scrambled screen came with. */
static void mdp5_note_others(u64 mdp, u32 ours) {
    for (int c = 0; c < 3; c++) {
        u64 cb = mdp + 0x1000 + 0x200 * (u64)c;
        for (int lm = 0; lm < 2; lm++) {
            u32 lay = R32(cb + 4 * (u64)lm), ext = R32(cb + 0x40 + 4 * (u64)lm);
            if (!lay && !ext) continue;
            klog("display: CTL%d mixer %d: layers %08x %08x", c, lm, lay, ext);
            for (int j = 0; j < 5; j++) {
                if (mdp_pipes[j].off == ours || !pipe_stage(lay, ext, j)) continue;
                u64 pb = mdp + mdp_pipes[j].off;
                u32 sz = R32(pb), ys = R32(pb + 0x24) & 0xffff, a = R32(pb + 0x14);
                if (fb_nother < 4 && a && ys && arm_is_ram(a, (u64)ys * (sz >> 16))) {
                    fb_other[fb_nother][0] = a; fb_other[fb_nother][1] = (u64)ys * (sz >> 16); fb_nother++;
                }
                klog("display: the boot loader's layer %x (stage %d, memory %x) stays; its memory is kept", mdp_pipes[j].off,
                     pipe_stage(lay, ext, j), a);
            }
        }
    }
}
static int mdp5_init(void) {
    if (fdt_find_compatible(-1, "qcom,mdp5") < 0) return 0;
    static const u32 pipes[] = { 0x04000, 0x14000, 0x16000, 0x24000, 0x34000 };
    static const u64 bases[] = { 0x01a01000, 0x01a00000 };
    for (int bi = 0; bi < 2; bi++)
        for (int pi = 0; pi < 5; pi++) {
            u64 p = bases[bi] + pipes[pi];
            u32 size = R32(p + 0x00), addr = R32(p + 0x14), stride = R32(p + 0x24) & 0xffff, format = R32(p + 0x30);
            u32 w = size & 0xffff, h = size >> 16, cpp = ((format >> 9) & 3) + 1;
            if (w < 320 || h < 320 || w > 4096 || h > 4096 || !addr || cpp < 3 || stride < w * cpp) continue;
            if (!arm_is_ram(addr, (u64)stride * h)) continue;           /* never draw outside RAM */
            /* byte i of a pixel holds channel unpack[i]: 0 G, 1 B, 2 R, 3 alpha */
            u32 unpack = R32(p + 0x34);
            byte_of[0] = byte_of[1] = byte_of[2] = -1;
            for (u32 i = 0; i < cpp; i++) {
                u32 c = unpack >> (8 * i) & 3;
                if (c == 2) byte_of[0] = (int)i; else if (c == 0) byte_of[1] = (int)i; else if (c == 1) byte_of[2] = (int)i;
            }
            if (byte_of[0] < 0 || byte_of[1] < 0 || byte_of[2] < 0) { byte_of[0] = 2; byte_of[1] = 1; byte_of[2] = 0; }
            fb = (u8 *)(usize)addr;
            bpp = (int)cpp;
            k.fb_w = w; k.fb_h = h; k.fb_stride = stride / cpp; k.fb_base = addr;
            fb_reserve_base = addr & ~0x1fffffull;
            fb_reserve_size = ((addr + (u64)stride * h + 0x1fffff) & ~0x1fffffull) - fb_reserve_base;
            /* no interface timing engine running: a command-mode panel, which shows a frame
             * only when told (CTL 0 START) */
            int video = 0;
            static const u32 intfs[] = { 0x6a000, 0x6a800, 0x6b000, 0x6b800 };
            for (int i = 0; i < 4; i++) if (R32(bases[bi] + intfs[i]) & 1) video = 1;
            /* the boot loader names the panel on the command line ("..._fhd_video",
             * "..._cmd"): that wins over the registers (the Mi A1's panels are all video) */
            int ch = fdt_node("/chosen"), al;
            const char *args = ch >= 0 ? fdt_prop(ch, "bootargs", &al) : NULL;
            const char *pn = args ? strstr(args, "mdss_dsi_") : NULL;
            if (pn) {
                const char *e = pn;
                while (*e && *e != ' ' && *e != ':' && *e != ',') e++;
                if (e - pn >= 6 && !strncmp(e - 6, "_video", 6)) video = 1;
                else if (e - pn >= 4 && !strncmp(e - 4, "_cmd", 4)) video = 0;
            }
            if (!video) kick = bases[bi] + 0x01000 + 0x1c;
            char panel[48] = "";
            if (pn) {
                int i = 0;
                for (const char *q = pn; *q && *q != ' ' && *q != ':' && *q != ',' && i < 47; q++) panel[i++] = *q;
                panel[i] = 0;
            }
            fmt(fb_what, sizeof fb_what, "boot splash (MDP5 pipe %x, %u-byte pixels, %s mode%s%s)", pipes[pi], cpp, video ? "video" : "command",
                panel[0] ? ", panel " : "", panel);
            u32 out = R32(p + 0x0c), oxy = R32(p + 0x10);
            klog("display: pipe %x: %ux%u from %x, out %ux%u at %u,%u, format %x", pipes[pi], w, h, addr, out & 0xffff, out >> 16,
                 oxy & 0xffff, oxy >> 16, format);
            mdp5_note_others(bases[bi], pipes[pi]);
            return 1;
        }
    return 0;
}

/* the panel is up but the pipe registers said nothing: the device tree's continuous
 * splash region, at the panel's size from qrt.panel=WxH (default 1080 x 1920, RGB888) */
static int splash_one(int c, void *arg) {
    u64 *r = arg, a, s;
    if (!strncmp(fdt_name(c), "cont-splash", 11) && fdt_reg(c, 0, &a, &s)) { r[0] = a; r[1] = s; return 1; }
    return 0;
}
static int splash_init(void) {
    int rm = fdt_node("/reserved-memory");
    u64 r[2] = { 0, 0 };
    if (rm < 0 || !fdt_children(rm, splash_one, r)) return 0;
    u32 w = 1080, h = 1920;
    if ((u64)w * h * 3 > r[1]) return 0;
    fb = (u8 *)(usize)r[0];
    bpp = 3; byte_of[0] = 2; byte_of[1] = 1; byte_of[2] = 0;
    k.fb_w = w; k.fb_h = h; k.fb_stride = w; k.fb_base = r[0];
    fb_reserve_base = r[0] & ~0x1fffffull;
    fb_reserve_size = ((r[0] + r[1] + 0x1fffff) & ~0x1fffffull) - fb_reserve_base;
    fmt(fb_what, sizeof fb_what, "continuous-splash memory (assumed 1080 x 1920 RGB888)");
    return 1;
}

int fb_init(void) { return ramfb_init() || mdp5_init() || splash_init(); }

/* Tessera's pixels are 0x00RRGGBB */
/* the framebuffer is uncached: a row is converted in cached memory first and stored with
 * 8-byte writes (byte stores to uncached memory are each a bus write) */
static void row_store(u8 *d, const u8 *s, usize n) {
    while (n && ((usize)d & 7)) { *d++ = *s++; n--; }
    for (; n >= 8; n -= 8, d += 8, s += 8) *(volatile u64 *)d = *(const u64 *)s;   /* s aligned like d */
    while (n--) *d++ = *s++;
}
static u64 fb_us, fb_frames;
/* a command-mode panel shows memory only when told (CTL START).  A frame takes ~15 ms to
 * go out, and a START during it cuts it short (the lower part of the screen kept an older
 * frame), while a START that is lost leaves the newest frame unseen.  So: START at most
 * every 40 ms while frames come, and once more 80 ms after the last one */
static volatile int fb_dirty;
static void refresher(void *a) {
    (void)a;
    u64 last_kick = 0, confirm_at = 0;
    for (;;) {
        u64 now = k_now_us();
        if (!kick) { thr_sleep_us(100000); continue; }               /* Linux's driver has the display now */
        if (fb_dirty && now - last_kick >= 40000) {
            fb_dirty = 0;
            W32(kick, 1);
            last_kick = now;
            confirm_at = now + 80000;
        } else if (confirm_at && now >= confirm_at && !fb_dirty) {
            W32(kick, 1);
            last_kick = now;
            confirm_at = 0;
        }
        thr_sleep_us(5000);
    }
}
/* ---- Argon: Linux's own display driver owns the panel; Tessera draws into a shadow
 * of Linux's framebuffer, and linux.c writes the changed part to /dev/fb0 ---- */
static volatile int dx0 = 1 << 30, dy0 = 1 << 30, dx1, dy1, redirected;
int fb_redirect(u8 *shadow, u32 w, u32 h, u32 stride_px, int r, int g, int b) {
    if (w != k.fb_w || h != k.fb_h) return -1;                       /* the shell is laid out for this size */
    u64 f = irq_save();
    kick = 0;                                                         /* Linux's display driver has the MDP */
    fb = shadow;
    bpp = 4;
    byte_of[0] = r; byte_of[1] = g; byte_of[2] = b;
    k.fb_stride = stride_px;
    k.fb_base = (u64)(usize)shadow;
    redirected = 1;
    irq_restore(f);
    fmt(fb_what, sizeof fb_what, "Linux's display driver (/dev/fb0), %u x %u", w, h);
    return 0;
}
/* the rectangle changed since the last call (0 if none) */
int fb_take_dirty(int *x, int *y, int *w, int *h) {
    u64 f = irq_save();
    int any = dx1 > dx0 && dy1 > dy0;
    if (any) { *x = dx0; *y = dy0; *w = dx1 - dx0; *h = dy1 - dy0; }
    dx0 = dy0 = 1 << 30; dx1 = dy1 = 0;
    irq_restore(f);
    return any;
}
static void note_dirty(int x, int y, int w, int h) {
    if (!redirected) return;
    u64 f = irq_save();
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
    irq_restore(f);
}

/* now, without the thread (the panic screen: no other thread runs again) */
void fb_flush(void) { if (kick) W32(kick, 1); }
void fb_stats(u64 *frames, u64 *us) { *frames = fb_frames; *us = fb_us; fb_frames = fb_us = 0; }
u32 fb_underruns(void) { return 0; }
void fb_linux_owns_mdp(void) { }
void fb_start_watch(void) { }
/* px is the whole frame (stride pixels a row); the rectangle (x, y, w, h) of it goes to
 * the same place on the screen - as GOP's Blt and the x86-64 kernel's native_present */
/* one band of rows of a present: converted in cached memory, stored with 8-byte writes.
 * Pure memory work, so the other cores take bands too (smp_run): a full 1080 x 1920 frame
 * took 20 ms on one core (0.26.0's log) */
static struct { const u32 *px; int stride, x, y, w, h; } pj;
static u8 rowbufs[16][4096 * 4 + 16] __attribute__((aligned(16)));
static void present_band(void *arg, int i, int n) {
    (void)arg;
    int ra = pj.h * i / n, rb = pj.h * (i + 1) / n;
    usize line = (usize)k.fb_stride * (usize)bpp;
    if (bpp == 4 && byte_of[0] == 2 && byte_of[1] == 1 && byte_of[2] == 0) {
        for (int r = ra; r < rb; r++)
            memcpy(fb + (usize)(pj.y + r) * line + (usize)pj.x * 4, pj.px + (usize)r * pj.stride, (usize)pj.w * 4);
        return;
    }
    u8 *rowbuf = rowbufs[i & 15];
    int r0 = byte_of[0], g0 = byte_of[1], b0 = byte_of[2];
    for (int r = ra; r < rb; r++) {
        u8 *d = fb + (usize)(pj.y + r) * line + (usize)pj.x * (usize)bpp;
        u8 *b = rowbuf + ((usize)d & 7), *o = b;                  /* the same alignment as d */
        const u32 *src = pj.px + (usize)r * pj.stride;
        int nn = MIN(pj.w, 4096);
        for (int c = 0; c < nn; c++, o += bpp) {
            u32 v = src[c];
            o[r0] = (u8)(v >> 16); o[g0] = (u8)(v >> 8); o[b0] = (u8)v;
        }
        row_store(d, b, (usize)nn * (usize)bpp);
    }
}
void fb_present(const u32 *px, int stride, int x, int y, int w, int h) {
    if (!fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)k.fb_w) w = (int)k.fb_w - x;
    if (y + h > (int)k.fb_h) h = (int)k.fb_h - y;
    if (w <= 0 || h <= 0) return;
    u64 t0 = k_now_us();
    pj.px = px + (usize)y * (usize)stride + (usize)x; pj.stride = stride; pj.x = x; pj.y = y; pj.w = w; pj.h = h;
    int bands = (u64)w * (u64)h >= 40000 ? MIN(16, MIN(h, 2 * (smp_workers() + 1))) : 1;
    if (bands > 1) smp_run(present_band, NULL, bands);
    else present_band(NULL, 0, 1);
    note_dirty(x, y, w, h);
    fb_us += k_now_us() - t0;
    fb_frames++;
    __asm__ volatile("dsb sy" ::: "memory");
    if (kick) {
        static int started;
        if (!started) { started = 1; W32(kick, 1); thr_create("display", refresher, NULL, 16 << 10); }
        else fb_dirty = 1;
    }
}
