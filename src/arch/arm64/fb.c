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

char fb_what[64] = "none";
u64 fb_reserve_base, fb_reserve_size;      /* mapped uncached by mmu.c */
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
    /* the buffer: the top of the first RAM bank (kept out of the heap as a hole) */
    int mem = fdt_node("/memory");
    u64 rb, rs;
    if (mem < 0 || !fdt_reg(mem, 0, &rb, &rs)) return 0;
    u64 bytes = ((u64)w * h * 4 + 0x1fffff) & ~0x1fffffull;
    u64 base = (rb + rs - bytes) & ~0x1fffffull;
    static struct __attribute__((packed)) { u64 addr; u32 fourcc, flags, width, height, stride; } rc;
    rc.addr = __builtin_bswap64(base);
    rc.fourcc = be32(0x34325258);                       /* XR24: XRGB8888 */
    rc.flags = 0;
    rc.width = be32(w); rc.height = be32(h); rc.stride = be32(w * 4);
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
    bpp = 4; byte_of[0] = 2; byte_of[1] = 1; byte_of[2] = 0;
    fmt(fb_what, sizeof fb_what, "QEMU ramfb");
    return 1;
}

/* ---- Qualcomm MDP5: the pipe the boot loader's splash screen scans out ---- */
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
            static const u32 intfs[] = { 0x6a000, 0x6a800, 0x6b000 };
            for (int i = 0; i < 3; i++) if (R32(bases[bi] + intfs[i]) & 1) video = 1;
            if (!video) kick = bases[bi] + 0x01000 + 0x1c;
            fmt(fb_what, sizeof fb_what, "boot splash (MDP5 pipe %x, %u-byte pixels, %s mode)", pipes[pi], cpp, video ? "video" : "command");
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
void fb_present(const u32 *px, int stride, int x, int y, int w, int h) {
    if (!fb) return;
    if (x < 0) { w += x; px -= x; x = 0; }
    if (y < 0) { h += y; px -= (isize)y * stride; y = 0; }
    if (x + w > (int)k.fb_w) w = (int)k.fb_w - x;
    if (y + h > (int)k.fb_h) h = (int)k.fb_h - y;
    if (w <= 0 || h <= 0) return;
    usize line = (usize)k.fb_stride * (usize)bpp;
    if (bpp == 4 && byte_of[0] == 2 && byte_of[1] == 1 && byte_of[2] == 0) {
        for (int r = 0; r < h; r++) memcpy(fb + (usize)(y + r) * line + (usize)x * 4, px + (usize)r * stride, (usize)w * 4);
    } else {
        int r0 = byte_of[0], g0 = byte_of[1], b0 = byte_of[2];
        for (int r = 0; r < h; r++) {
            u8 *d = fb + (usize)(y + r) * line + (usize)x * (usize)bpp;
            const u32 *s = px + (usize)r * stride;
            for (int c = 0; c < w; c++, d += bpp) {
                u32 v = s[c];
                d[r0] = (u8)(v >> 16); d[g0] = (u8)(v >> 8); d[b0] = (u8)v;
            }
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (kick) W32(kick, 1);
}
