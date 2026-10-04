/*
 * ivb.c - Intel Ivy Bridge graphics (HD Graphics 2500/4000, the Panasonic FZ-G1's):
 * the blitter engine puts the shell's picture on the panel, and the display engine
 * mirrors it to an HDMI monitor.
 *
 * GPU.  The firmware lit the eDP panel (pipe A) from a linear framebuffer in the GTT
 * aperture.  Here the blitter command streamer (BCS) copies each damaged rectangle of
 * the shell's canvas into it with XY_SRC_COPY_BLT, as Linux's i915 and the X server's
 * SNA did on this generation: forcewake (intel_uncore.c, the multi-threaded variant),
 * our pages in the global GTT with LLC caching (gen6 PTEs, i915_gem_gtt.c: coherent with
 * the CPU caches, no flushes), a legacy ring on BCS (intel_ringbuffer.c init_ring_common)
 * and MI_FLUSH_DW with a post-sync write as the fence.  A self-test copies a pattern and
 * checks every pixel; a failure, or a copy that does not finish in 100 ms, leaves the
 * CPU drawing for good, and a start that never came back is remembered in NVRAM
 * (QrtGpuGuard) so the next boot skips the GPU.
 *
 * HDMI.  A thread reads EDID over GMBUS (intel_i2c.c) on the chipset's HDMI ports B, C
 * and D every two seconds.  When a monitor answers, it does what Linux's
 * ironlake_crtc_enable does for a PCH HDMI port on pipe B: the PCH DPLL (computed as
 * i9xx_find_best_dpll with the Ironlake DAC limits, 120 MHz reference), pipe timings and
 * FDI M/N, pipe B, the FDI PLLs, the panel fitter, the pipe, FDI link training
 * (ivb_manual_fdi_link_train), the PCH transcoder and finally the HDMI port.  Plane B
 * scans out the panel's own framebuffer, and the panel fitter scales it to the monitor's
 * mode (keeping its shape), so the monitor mirrors the tablet with no copies at all.
 */
#include "ivb.h"
#include "../../kernel/kernel.h"

#if defined(__x86_64__)
#include "../../arch/x64/mm.h"
#include "../../arch/x64/sched.h"

#define LOG(...) klog("ivb: " __VA_ARGS__)

/* ---- registers (i915_reg.h, v4.4) ------------------------------------------------------- */
#define FORCEWAKE           0xa18c
#define FORCEWAKE_ACK       0x130090
#define FORCEWAKE_MT        0xa188
#define FORCEWAKE_MT_ACK    0x130040
#define ECOBUS              0xa180
#define FORCEWAKE_MT_ENABLE (1u << 5)
#define GFX_FLSH_CNTL_GEN6  0x101008
#define MASKED_ENABLE(b)    ((b) << 16 | (b))
#define MASKED_DISABLE(b)   ((b) << 16)

#define BCS                 0x22000
#define RING_TAIL           (BCS + 0x30)
#define RING_HEAD           (BCS + 0x34)
#define RING_START          (BCS + 0x38)
#define RING_CTL            (BCS + 0x3c)
#define RING_MI_MODE        (BCS + 0x9c)
#define BLT_HWS_PGA_GEN7    0x04280
#define RING_VALID          1u
#define RING_NR_PAGES       0x001ff000u
#define STOP_RING           (1u << 8)
#define MODE_IDLE           (1u << 9)

#define MI_NOOP             0
#define MI_FLUSH_DW         ((0x26u << 23) | 1)
#define MI_FLUSH_DW_OP_STOREDW (1u << 14)
#define MI_INVALIDATE_TLB   (1u << 18)
#define MI_FLUSH_DW_USE_GTT (1u << 2)
#define XY_SRC_COPY_BLT_CMD ((2u << 29) | (0x53u << 22) | 6)
#define BLT_WRITE_ALPHA     (1u << 21)
#define BLT_WRITE_RGB       (1u << 20)
#define XY_DST_TILED        (1u << 11)

#define GEN6_PTE_VALID      1u
#define GEN6_PTE_CACHE_LLC  (2u << 1)

#define DSPACNTR            0x70180
#define DSPASTRIDE          0x70188
#define DSPASURF            0x7019c
#define DSPBCNTR            0x71180
#define DSPBADDR            0x71184
#define DSPBSTRIDE          0x71188
#define DSPBSURF            0x7119c
#define DSPBTILEOFF         0x711a4
#define DISPLAY_PLANE_ENABLE (1u << 31)
#define DISPPLANE_BGRX888   (6u << 26)
#define DISPPLANE_TILED     (1u << 10)
#define DISPPLANE_TRICKLE_FEED_DISABLE (1u << 14)

/* pipe / transcoder B */
#define HTOTAL_B            0x61000
#define HBLANK_B            0x61004
#define HSYNC_B             0x61008
#define VTOTAL_B            0x6100c
#define VBLANK_B            0x61010
#define VSYNC_B             0x61014
#define PIPEBSRC            0x6101c
#define VSYNCSHIFT_B        0x61028
#define PIPEB_DATA_M1       0x61030
#define PIPEB_DATA_N1       0x61034
#define PIPEB_LINK_M1       0x61040
#define PIPEB_LINK_N1       0x61044
#define PIPEBCONF           0x71008
#define PIPECONF_ENABLE     (1u << 31)
#define PIPECONF_STATE      (1u << 30)
#define PIPECONF_BPC_MASK   (7u << 5)
#define PFB_CTL             0x68880
#define PFB_WIN_POS         0x68870
#define PFB_WIN_SZ          0x68874
#define PF_ENABLE           (1u << 31)
#define PF_FILTER_MED_3x3   (1u << 23)
#define PF_PIPE_SEL_IVB(p)  ((u32)(p) << 29)
#define WM0_PIPEB_ILK       0x45104
#define WM1_LP_ILK          0x45108
#define WM2_LP_ILK          0x4510c
#define WM3_LP_ILK          0x45110

/* FDI B */
#define FDI_TXB_CTL         0x61100
#define FDI_RXB_CTL         0xf100c
#define FDI_RXB_MISC        0xf1010
#define FDI_RXB_IIR         0xf1014
#define FDI_RXB_IMR         0xf1018
#define FDI_RXB_TUSIZE1     0xf1030
#define FDI_TX_ENABLE       (1u << 31)
#define FDI_RX_ENABLE       (1u << 31)
#define FDI_DP_PORT_WIDTH_MASK (7u << 19)
#define FDI_DP_PORT_WIDTH(w) (((u32)(w) - 1) << 19)
#define FDI_RX_PLL_ENABLE   (1u << 13)
#define FDI_PCDCLK          (1u << 4)
#define FDI_TX_PLL_ENABLE   (1u << 14)
#define FDI_LINK_TRAIN_AUTO (1u << 10)
#define FDI_LINK_TRAIN_NONE_IVB (3u << 8)
#define FDI_LINK_TRAIN_PATTERN_1_IVB (0u << 8)
#define FDI_LINK_TRAIN_PATTERN_2_IVB (1u << 8)
#define FDI_LINK_TRAIN_PATTERN_MASK_CPT (3u << 8)
#define FDI_LINK_TRAIN_PATTERN_1_CPT (0u << 8)
#define FDI_LINK_TRAIN_PATTERN_2_CPT (1u << 8)
#define FDI_LINK_TRAIN_NORMAL_CPT (3u << 8)
#define FDI_LINK_TRAIN_VOL_EMP_MASK (0x3fu << 22)
#define FDI_COMPOSITE_SYNC  (1u << 11)
#define FDI_TX_ENHANCE_FRAME_ENABLE (1u << 18)
#define FDI_RX_ENHANCE_FRAME_ENABLE (1u << 6)
#define FDI_FS_ERRC_ENABLE  (1u << 27)
#define FDI_FE_ERRC_ENABLE  (1u << 26)
#define FDI_RX_TP1_TO_TP2_48 (2u << 20)
#define FDI_RX_FDI_DELAY_90 0x90u
#define FDI_RX_BIT_LOCK     (1u << 8)
#define FDI_RX_SYMBOL_LOCK  (1u << 9)
#define TU_SIZE(x)          (((u32)(x) - 1) << 25)
#define TU_SIZE_MASK        (0x3fu << 25)
#define SOUTH_CHICKEN1      0xc2000
#define FDI_BC_BIFURCATION_SELECT (1u << 12)

/* PCH: DPLL, transcoder B, HDMI ports, GMBUS */
#define PCH_DREF_CONTROL    0xc6200
#define DREF_NONSPREAD_SOURCE_MASK   (3u << 9)
#define DREF_NONSPREAD_SOURCE_ENABLE (2u << 9)
#define PCH_DPLL_A          0xc6014
#define PCH_FPA0            0xc6040
#define PCH_FPA1            0xc6044
#define PCH_DPLL_SEL        0xc7000
#define TRANS_DPLL_ENABLE(p) (1u << ((p) * 4 + 3))
#define TRANS_DPLLB_SEL(p)  (1u << ((p) * 4))
#define DPLL_VCO_ENABLE     (1u << 31)
#define DPLL_SDVO_HIGH_SPEED (1u << 30)
#define DPLLB_MODE_DAC_SERIAL (1u << 26)
#define DPLL_DAC_SERIAL_P2_CLOCK_DIV_5 (1u << 24)
#define FP_CB_TUNE          (3u << 22)
#define PCH_TRANS_HTOTAL_B  0xe1000
#define PCH_TRANS_HBLANK_B  0xe1004
#define PCH_TRANS_HSYNC_B   0xe1008
#define PCH_TRANS_VTOTAL_B  0xe100c
#define PCH_TRANS_VBLANK_B  0xe1010
#define PCH_TRANS_VSYNC_B   0xe1014
#define PCH_TRANS_VSYNCSHIFT_B 0xe1028
#define PCH_TRANSBCONF      0xf1008
#define TRANSB_CHICKEN2     0xf1064
#define TRANS_ENABLE        (1u << 31)
#define TRANS_STATE_ENABLE  (1u << 30)
#define TRANS_CHICKEN2_TIMING_OVERRIDE (1u << 31)
#define SDVO_ENABLE         (1u << 31)
#define SDVO_ENCODING_HDMI  (2u << 10)
#define HDMI_MODE_SELECT_HDMI (1u << 9)
#define SDVO_VSYNC_ACTIVE_HIGH (1u << 4)
#define SDVO_HSYNC_ACTIVE_HIGH (1u << 3)
#define SDVO_PIPE_SEL_CPT(p) ((u32)(p) << 29)
#define PCH_GMBUS0          0xc5100
#define PCH_GMBUS1          0xc5104
#define PCH_GMBUS2          0xc5108
#define PCH_GMBUS3          0xc510c
#define PCH_GMBUS4          0xc5110
#define PCH_GMBUS5          0xc5120
#define GMBUS_SW_CLR_INT    (1u << 31)
#define GMBUS_SW_RDY        (1u << 30)
#define GMBUS_CYCLE_WAIT    (1u << 25)
#define GMBUS_CYCLE_INDEX   (2u << 25)
#define GMBUS_CYCLE_STOP    (4u << 25)
#define GMBUS_HW_WAIT_PHASE (1u << 14)
#define GMBUS_HW_RDY        (1u << 11)
#define GMBUS_SATOER        (1u << 10)
#define GMBUS_ACTIVE        (1u << 9)

enum { S_NONE, S_OFF, S_READY, S_FAILED };

#define ARENA_SIZE   0x10000
#define A_RING       0x0000
#define RING_SIZE    0x4000
#define A_HWS        0x4000                   /* status page; our fence dword at +0x40 */
#define A_TSRC       0x5000                   /* self-test: 32x32 source, then destination */
#define A_TDST       0x6000
#define SCENE_WINDOW (48u << 20)              /* GGTT room for two canvases */
#define SLOT_SIZE    (SCENE_WINDOW / 2)

static struct {
    int state;
    char status[112];
    pci_dev_t *pci;
    volatile u8 *mmio;
    volatile u32 *gsm;
    u64 gmadr, ggtt_size;
    u32 fb_off, fb_tiled;
    u8 *arena;
    u32 arena_gtt, scene_gtt;
    struct { const u8 *page; usize pages; u32 used; } slot[2];
    u32 clock, tail, seqno;
    int guard;
    u32 frames;
    u64 busy_us;
} g;

static u32 rd(u32 r) { return *(volatile u32 *)(g.mmio + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(g.mmio + r) = v; }
static int wait_reg(u32 r, u32 mask, u32 want, u32 ms) {
    u64 end = k_now_us() + (u64)ms * 1000;
    while ((rd(r) & mask) != want) if (k_now_us() > end) return 0;
    return 1;
}

int ivb_matches(const pci_dev_t *d) {
    if (d->vendor != 0x8086) return 0;
    switch (d->device) {
    case 0x0152: case 0x0156: case 0x015a: case 0x0162: case 0x0166: case 0x016a: return 1;
    }
    return 0;
}

static const char *map_regs(void) {
    if (g.mmio) return NULL;
    pci_dev_t *d = g.pci;
    u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
    pci_write32(d->bus, d->dev, d->fn, 4, cmd | 0x6);
    u64 bar0 = pci_bar(d->bus, d->dev, d->fn, 0);
    g.gmadr = pci_bar(d->bus, d->dev, d->fn, 2);
    if (!bar0) return "no register BAR";
    u16 gmch = pci_read16(d->bus, d->dev, d->fn, 0x50);              /* SNB_GMCH_CTRL: GGMS */
    u32 ggms = (gmch >> 8) & 3;
    if (!ggms) return "no GTT";
    g.mmio = mm_map_mmio(bar0, 4u << 20);
    if (!g.mmio) return "registers could not be mapped";
    g.gsm = (volatile u32 *)(g.mmio + (2u << 20));                     /* gen6/7: the GTT in the upper half */
    g.ggtt_size = (u64)((ggms << 20) / 4) << 12;                      /* 4-byte entries, 4 KiB pages */
    return NULL;
}

/* =============================== the blitter ============================================= */
static void fail(const char *why) {
    g.state = S_FAILED;
    fmt(g.status, sizeof g.status, "Off: %s (CPU drawing)", why);
    LOG("%s", g.status);
    if (g.guard) { hal_setting_set(u"QrtGpuGuard", 0); g.guard = 0; }
}

static void gtt_map(u32 off, u64 phys, usize pages) {
    volatile u32 *pte = g.gsm + (off >> 12);
    for (usize i = 0; i < pages; i++) {
        u64 a = phys + i * 4096;
        pte[i] = (u32)(a | ((a >> 28) & 0xff0)) | GEN6_PTE_CACHE_LLC | GEN6_PTE_VALID;
    }
    (void)pte[pages - 1];
    wr(GFX_FLSH_CNTL_GEN6, 1);
    (void)rd(GFX_FLSH_CNTL_GEN6);
}

static int forcewake(void) {                   /* __gen7_gt_force_wake_mt_get, or the legacy one */
    if (rd(ECOBUS) & FORCEWAKE_MT_ENABLE) {
        wait_reg(FORCEWAKE_MT_ACK, 1, 0, 50);
        wr(FORCEWAKE_MT, MASKED_ENABLE(1u));
        return wait_reg(FORCEWAKE_MT_ACK, 1, 1, 50);
    }
    wait_reg(FORCEWAKE_ACK, 1, 0, 50);
    wr(FORCEWAKE, 1);
    return wait_reg(FORCEWAKE_ACK, 1, 1, 50);
}

static u32 *ring(void) { return (u32 *)(g.arena + A_RING); }
static void ring_begin(u32 dwords) {
    if (g.tail + dwords * 4 > RING_SIZE - 8) {                        /* pad to the end, wrap */
        while (g.tail < RING_SIZE) { ring()[g.tail / 4] = MI_NOOP; g.tail += 4; }
        g.tail = 0;
    }
}
static void out(u32 v) { ring()[g.tail / 4] = v; g.tail += 4; }

static int ring_init(void) {                   /* init_ring_common */
    wr(RING_MI_MODE, MASKED_ENABLE(STOP_RING));
    wait_reg(RING_MI_MODE, MODE_IDLE, MODE_IDLE, 100);
    wr(RING_CTL, 0);
    wr(RING_HEAD, 0);
    wr(RING_TAIL, 0);
    wr(BLT_HWS_PGA_GEN7, g.arena_gtt + A_HWS);
    wr(RING_START, g.arena_gtt + A_RING);
    wr(RING_CTL, ((RING_SIZE - 4096) & RING_NR_PAGES) | RING_VALID);
    wr(RING_MI_MODE, MASKED_DISABLE(STOP_RING));
    if (!wait_reg(RING_CTL, RING_VALID, RING_VALID, 50) || (rd(RING_HEAD) & 0x1ffffc) != 0 || rd(RING_START) != g.arena_gtt + A_RING) {
        LOG("ring did not start: ctl %08x head %08x start %08x", rd(RING_CTL), rd(RING_HEAD), rd(RING_START));
        return 0;
    }
    g.tail = 0;
    return 1;
}

/* end a submission with a flush that writes the sequence number, then wait for it */
static int submit_and_wait(const char *what) {
    volatile u32 *fence = (volatile u32 *)(g.arena + A_HWS + 0x40);
    u32 seq = ++g.seqno;
    ring_begin(4);
    out(MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW | MI_INVALIDATE_TLB);
    out((g.arena_gtt + A_HWS + 0x40) | MI_FLUSH_DW_USE_GTT);
    out(seq);
    out(MI_NOOP);
    __asm__ volatile("mfence" ::: "memory");
    wr(RING_TAIL, g.tail);
    u64 t0 = k_now_us(), end = t0 + 100000;
    while (*fence != seq) {
        if (k_now_us() > end) {
            LOG("%s did not finish: head %08x tail %08x", what, rd(RING_HEAD), g.tail);
            fail("the blitter stopped answering");
            return 0;
        }
        __asm__ volatile("pause");
    }
    g.busy_us += k_now_us() - t0;
    return 1;
}

static void blit(u32 src, int spitch, int sx, int sy, u32 dst, int dpitch, int dtiled, int dx, int dy, int w, int h) {
    ring_begin(8);
    out(XY_SRC_COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB | (dtiled ? XY_DST_TILED : 0));
    out(3u << 24 | 0xccu << 16 | (u32)(dtiled ? dpitch / 4 : dpitch));
    out((u32)dy << 16 | (u32)dx);
    out((u32)(dy + h) << 16 | (u32)(dx + w));
    out(dst);
    out((u32)sy << 16 | (u32)sx);
    out((u32)spitch);
    out(src);
}

static int self_test(void) {
    u32 *s = (u32 *)(g.arena + A_TSRC), *d = (u32 *)(g.arena + A_TDST);
    for (int i = 0; i < 32 * 32; i++) { s[i] = 0x01000193u * (u32)(i + 7); d[i] = 0; }
    blit(g.arena_gtt + A_TSRC, 32 * 4, 0, 0, g.arena_gtt + A_TDST, 32 * 4, 0, 0, 0, 32, 32);
    if (!submit_and_wait("the self-test")) return 0;
    for (int i = 0; i < 32 * 32; i++)
        if (d[i] != s[i]) { LOG("self-test: pixel %d is %08x, not %08x", i, d[i], s[i]); return 0; }
    return 1;
}

static int bring_up(void) {
    const char *why = map_regs();
    if (why) { fail(why); return 0; }
    if (!k.fb_base || !g.gmadr || k.fb_base < g.gmadr || k.fb_base - g.gmadr >= (1u << 30)) { fail("framebuffer is not in the GPU aperture"); return 0; }
    if (k.fb_rgb) { fail("RGB framebuffer (the blitter copies BGR)"); return 0; }
    g.fb_off = (u32)(k.fb_base - g.gmadr);
    u32 acntr = rd(DSPACNTR), asurf = rd(DSPASURF);
    g.fb_tiled = (acntr & DISPPLANE_TILED) != 0;
    LOG("GTT %llu MB, aperture at %llx, framebuffer at GTT %x (%s), plane A %08x @%x",
        g.ggtt_size >> 20, g.gmadr, g.fb_off, g.fb_tiled ? "X-tiled" : "linear", acntr, asurf);
    if (g.ggtt_size < (128u << 20)) { fail("GTT too small"); return 0; }
    if (!forcewake()) { fail("the GT did not wake"); return 0; }
    g.arena = hal_dma_alloc(ARENA_SIZE);
    if (!g.arena) { fail("out of memory"); return 0; }
    g.arena_gtt = (u32)(g.ggtt_size - (1u << 20));
    g.scene_gtt = g.arena_gtt - SCENE_WINDOW;
    gtt_map(g.arena_gtt, (u64)(usize)g.arena, ARENA_SIZE / 4096);
    if (!ring_init()) { fail("the blitter ring did not start"); return 0; }
    if (!self_test()) { if (g.state != S_FAILED) fail("self-test picture was wrong"); return 0; }
    return 1;
}

int ivb_probe(pci_dev_t *d) {
    if (!k.native || !ivb_matches(d)) return 0;
    if (g.pci) return g.state == S_READY;
    g.pci = d;
    if (hal_setting_get(u"QrtGpuGuard", 0)) {
        hal_setting_set(u"QrtGpuGuard", 0);
        g.state = S_OFF;
        strlcpy(g.status, "Off for this boot: the last start did not finish (on again next boot)", sizeof g.status);
        LOG("%s", g.status);
        map_regs();
        return 0;
    }
    if (hal_setting_get(u"QrtGpuMode", 0) == 1) {
        g.state = S_OFF;
        strlcpy(g.status, "Off (CPU drawing)", sizeof g.status);
        map_regs();
        return 0;
    }
    hal_setting_set(u"QrtGpuGuard", 1);
    g.guard = 1;
    u64 t0 = k_now_us();
    if (!bring_up()) return 0;
    g.state = S_READY;
    strlcpy(g.status, "On: the blitter draws the screen (Ivy Bridge)", sizeof g.status);
    LOG("blitter up in %llu us, self-test passed", k_now_us() - t0);
    return 1;
}

int ivb_present_supported(void) { return g.pci != NULL; }
int ivb_active(void) { return g.state == S_READY; }
const char *ivb_status(void) { return g.pci ? g.status : "not an Ivy Bridge GPU"; }
void ivb_set_enabled(int on) {
    if (!g.pci) return;
    hal_setting_set(u"QrtGpuMode", on ? 0 : 1);
    if (!on) { if (g.state == S_READY) g.state = S_OFF; strlcpy(g.status, "Off (CPU drawing)", sizeof g.status); }
    else if (g.state == S_OFF) {
        if (g.arena) { g.state = S_READY; strlcpy(g.status, "On: the blitter draws the screen (Ivy Bridge)", sizeof g.status); }
        else { pci_dev_t *d = g.pci; g.pci = NULL; ivb_probe(d); }
    }
}
void ivb_stats(u32 *frames, u32 *avg_us) { *frames = g.frames; *avg_us = g.frames ? (u32)(g.busy_us / g.frames) : 0; }

static u32 map_canvas(const u32 *src, usize bytes) {
    const u8 *first = (const u8 *)((usize)src & ~(usize)4095);
    usize pages = ((usize)src + bytes - (usize)first + 4095) / 4096;
    if (pages > SLOT_SIZE / 4096) return 0;
    int s;
    for (s = 0; s < 2; s++) if (g.slot[s].page == first && g.slot[s].pages == pages) goto found;
    s = g.slot[0].used <= g.slot[1].used ? 0 : 1;
    gtt_map(g.scene_gtt + (u32)s * SLOT_SIZE, (u64)(usize)first, pages);
    g.slot[s].page = first;
    g.slot[s].pages = pages;
found:
    g.slot[s].used = ++g.clock;
    return g.scene_gtt + (u32)s * SLOT_SIZE + (u32)((usize)src & 4095);
}

int ivb_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h) {
    if (g.state != S_READY || rot || w <= 0 || h <= 0) return 0;
    if (stride * 4 > 32767 || (int)k.fb_stride * 4 > 32767) return 0;
    u32 s = map_canvas(src, (usize)stride * 4 * (usize)sh);
    if (!s) return 0;
    blit(s, stride * 4, x, y, g.fb_off, (int)k.fb_stride * 4, g.fb_tiled, x, y, w, h);
    if (!submit_and_wait("a frame")) return 0;
    if (++g.frames == 20 && g.guard) { hal_setting_set(u"QrtGpuGuard", 0); g.guard = 0; }
    return 1;
}

/* =============================== HDMI ===================================================== */
typedef struct { int clock, hdisplay, hsync_start, hsync_end, htotal, vdisplay, vsync_start, vsync_end, vtotal, phsync, pvsync; } hmode_t;

static struct {
    char status[128], monitor[16];
    int port;                       /* 0 none; 1 HDMI B, 2 C, 3 D */
    int up, misses;
    hmode_t m;
} H;

static const u32 hdmi_reg[4] = { 0, 0xe1140, 0xe1150, 0xe1160 };
static const u32 gmbus_pin[4] = { 0, 5, 4, 6 };

static void gmbus_reset(void) {
    wr(PCH_GMBUS1, GMBUS_SW_CLR_INT);
    wr(PCH_GMBUS1, 0);
    wr(PCH_GMBUS0, 0);
}

/* read len bytes at offset off from the DDC address 0x50 behind this port (gmbus_xfer_index_read) */
static int ddc_read(int port, u8 off, u8 *buf, int len) {
    wr(PCH_GMBUS0, gmbus_pin[port]);                                   /* 100 kHz */
    wr(PCH_GMBUS1, GMBUS_CYCLE_INDEX | (u32)off << 8 | GMBUS_CYCLE_WAIT | (u32)len << 16 | 0x50u << 1 | 1u | GMBUS_SW_RDY);
    for (int i = 0; i < len; ) {
        u64 end = k_now_us() + 50000;
        u32 st;
        while (!((st = rd(PCH_GMBUS2)) & (GMBUS_HW_RDY | GMBUS_SATOER))) if (k_now_us() > end) { gmbus_reset(); return -1; }
        if (st & GMBUS_SATOER) { gmbus_reset(); return -1; }
        u32 v = rd(PCH_GMBUS3);
        for (int k2 = 0; k2 < 4 && i < len; k2++, i++) { buf[i] = (u8)v; v >>= 8; }
    }
    u64 end = k_now_us() + 10000;
    while (!(rd(PCH_GMBUS2) & GMBUS_HW_WAIT_PHASE) && k_now_us() < end) {}
    wr(PCH_GMBUS1, GMBUS_CYCLE_STOP | GMBUS_SW_RDY);
    end = k_now_us() + 10000;
    while ((rd(PCH_GMBUS2) & GMBUS_ACTIVE) && k_now_us() < end) {}
    wr(PCH_GMBUS0, 0);
    return 0;
}

static int read_edid(int port, u8 *e) {
    if (ddc_read(port, 0, e, 128)) return -1;
    static const u8 hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (memcmp(e, hdr, 8)) return -1;
    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum += e[i];
    return sum ? -1 : 0;
}

static int dtd(const u8 *b, hmode_t *m) {
    int clock = (b[0] | b[1] << 8) * 10;
    if (!clock || (b[17] & 0x80)) return 0;                          /* not a timing, or interlaced */
    int ha = b[2] | (b[4] >> 4) << 8, hb = b[3] | (b[4] & 15) << 8;
    int va = b[5] | (b[7] >> 4) << 8, vb = b[6] | (b[7] & 15) << 8;
    int hso = b[8] | ((b[11] >> 6) & 3) << 8, hsw = b[9] | ((b[11] >> 4) & 3) << 8;
    int vso = (b[10] >> 4) | ((b[11] >> 2) & 3) << 4, vsw = (b[10] & 15) | (b[11] & 3) << 4;
    *m = (hmode_t){ clock, ha, ha + hso, ha + hso + hsw, ha + hb, va, va + vso, va + vso + vsw, va + vb,
                   (b[17] & 0x18) == 0x18 && (b[17] & 2), (b[17] & 0x18) == 0x18 && (b[17] & 4) };
    return ha > 0 && va > 0;
}

static void pick_mode(const u8 *e) {
    hmode_t best = { 0 }, m;
    for (int i = 0; i < 4; i++)                                      /* the first timing is the preferred one */
        if (dtd(e + 54 + i * 18, &m) && m.clock <= 225000 && (!best.clock || i == 0)) { best = m; if (i == 0) break; }
    if (!best.clock) best = (hmode_t){ 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1 };   /* CEA 1080p60 */
    H.m = best;
    H.monitor[0] = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *d = e + 54 + i * 18;
        if (d[0] || d[1] || d[3] != 0xfc) continue;
        int n = 0;
        for (int j = 5; j < 18 && d[j] != 0x0a && n < (int)sizeof H.monitor - 1; j++) H.monitor[n++] = (char)d[j];
        H.monitor[n] = 0;
    }
    if (!H.monitor[0]) strlcpy(H.monitor, "HDMI monitor", sizeof H.monitor);
}

/* i9xx_find_best_dpll with intel_limits_ironlake_dac, 120 MHz reference */
typedef struct { int n, m1, m2, p1, p2, m, p, vco, dot; } clk_t;
static int find_dpll(int target, clk_t *best) {
    const int ref = 120000;
    int err = target;
    clk_t c;
    memset(best, 0, sizeof *best);
    c.p2 = target < 225000 ? 10 : 5;
    for (c.m1 = 12; c.m1 <= 22; c.m1++)
        for (c.m2 = 5; c.m2 <= 9 && c.m2 < c.m1; c.m2++)
            for (c.n = 1; c.n <= 5; c.n++)
                for (c.p1 = 1; c.p1 <= 8; c.p1++) {
                    c.m = 5 * (c.m1 + 2) + (c.m2 + 2);
                    c.p = c.p1 * c.p2;
                    c.vco = (ref * c.m + (c.n + 2) / 2) / (c.n + 2);
                    c.dot = (c.vco + c.p / 2) / c.p;
                    if (c.m < 79 || c.m > 127 || c.p < 5 || c.p > 80 || c.vco < 1760000 || c.vco > 3510000 || c.dot < 25000 || c.dot > 350000) continue;
                    int e = c.dot > target ? c.dot - target : target - c.dot;
                    if (e < err) { *best = c; err = e; }
                }
    return err != target;
}

static void reduce(u32 *m, u32 *n) { while (*m > 0xffffff || *n > 0xffffff) { *m >>= 1; *n >>= 1; } }
static void compute_m_n(u64 m, u64 n, u32 *rm, u32 *rn) {     /* compute_m_n: N a power of two up to 0x800000 */
    u64 rn64 = 1;
    while (rn64 < n) rn64 <<= 1;
    if (rn64 > 0x800000) rn64 = 0x800000;
    *rn = (u32)rn64;
    *rm = (u32)(m * rn64 / n);
    reduce(rm, rn);
}

static void say(const char *f, const char *a) { fmt(H.status, sizeof H.status, f, a); LOG("hdmi: %s", H.status); }

static void hdmi_disable(void) {
    u32 hr = hdmi_reg[H.port];
    wr(hr, rd(hr) & ~SDVO_ENABLE);
    wr(DSPBCNTR, 0); wr(DSPBSURF, 0);
    wr(PIPEBCONF, rd(PIPEBCONF) & ~PIPECONF_ENABLE);
    wait_reg(PIPEBCONF, PIPECONF_STATE, 0, 100);
    wr(PFB_CTL, 0);
    wr(PCH_TRANSBCONF, rd(PCH_TRANSBCONF) & ~TRANS_ENABLE);
    wait_reg(PCH_TRANSBCONF, TRANS_STATE_ENABLE, 0, 100);
    wr(FDI_TXB_CTL, rd(FDI_TXB_CTL) & ~FDI_TX_ENABLE);
    wr(FDI_RXB_CTL, rd(FDI_RXB_CTL) & ~FDI_RX_ENABLE);
    wr(PCH_DPLL_SEL, rd(PCH_DPLL_SEL) & ~TRANS_DPLL_ENABLE(1));
    wr(PCH_DPLL_A, rd(PCH_DPLL_A) & ~DPLL_VCO_ENABLE);
    H.up = 0;
}

static int fdi_train(int lanes) {               /* ivb_manual_fdi_link_train */
    static const u32 vswing[4] = { 0x0u << 22, 0x3au << 22, 0x39u << 22, 0x38u << 22 };
    wr(FDI_RXB_IMR, rd(FDI_RXB_IMR) & ~(FDI_RX_SYMBOL_LOCK | FDI_RX_BIT_LOCK));
    hal_delay_us(150);
    for (int j = 0; j < 8; j++) {
        u32 t = rd(FDI_TXB_CTL) & ~(FDI_LINK_TRAIN_AUTO | FDI_LINK_TRAIN_NONE_IVB | FDI_TX_ENABLE);
        wr(FDI_TXB_CTL, t);
        u32 r = rd(FDI_RXB_CTL) & ~(FDI_LINK_TRAIN_AUTO | FDI_LINK_TRAIN_PATTERN_MASK_CPT | FDI_RX_ENABLE);
        wr(FDI_RXB_CTL, r);
        t = rd(FDI_TXB_CTL) & ~(FDI_DP_PORT_WIDTH_MASK | FDI_LINK_TRAIN_VOL_EMP_MASK);
        t |= FDI_DP_PORT_WIDTH(lanes) | FDI_LINK_TRAIN_PATTERN_1_IVB | vswing[j / 2] | FDI_COMPOSITE_SYNC;
        wr(FDI_TXB_CTL, t | FDI_TX_ENABLE);
        wr(FDI_RXB_MISC, FDI_RX_TP1_TO_TP2_48 | FDI_RX_FDI_DELAY_90);
        wr(FDI_RXB_CTL, rd(FDI_RXB_CTL) | FDI_LINK_TRAIN_PATTERN_1_CPT | FDI_COMPOSITE_SYNC | FDI_RX_ENABLE);
        (void)rd(FDI_RXB_CTL);
        hal_delay_us(1);
        int ok = 0;
        for (int i = 0; i < 4 && !ok; i++) {
            u32 iir = rd(FDI_RXB_IIR);
            if ((iir | rd(FDI_RXB_IIR)) & FDI_RX_BIT_LOCK) { wr(FDI_RXB_IIR, iir | FDI_RX_BIT_LOCK); ok = 1; }
            else hal_delay_us(1);
        }
        if (!ok) continue;
        wr(FDI_TXB_CTL, (rd(FDI_TXB_CTL) & ~FDI_LINK_TRAIN_NONE_IVB) | FDI_LINK_TRAIN_PATTERN_2_IVB);
        wr(FDI_RXB_CTL, (rd(FDI_RXB_CTL) & ~FDI_LINK_TRAIN_PATTERN_MASK_CPT) | FDI_LINK_TRAIN_PATTERN_2_CPT);
        (void)rd(FDI_RXB_CTL);
        hal_delay_us(2);
        for (int i = 0; i < 4; i++) {
            u32 iir = rd(FDI_RXB_IIR);
            if ((iir | rd(FDI_RXB_IIR)) & FDI_RX_SYMBOL_LOCK) { wr(FDI_RXB_IIR, iir | FDI_RX_SYMBOL_LOCK); return 1; }
            hal_delay_us(2);
        }
    }
    return 0;
}

static int hdmi_enable(void) {
    const hmode_t *m = &H.m;
    u32 fbw = k.fb_w, fbh = k.fb_h;
    clk_t c;
    if (!find_dpll(m->clock, &c)) { say("no PLL setting for %s", "this mode"); return 0; }
    int lanes = (int)(((u64)m->clock * 24 * 21 / 20 + 270000 * 8 - 1) / (270000 * 8));   /* ironlake_get_lanes_required */
    if (lanes < 1) lanes = 1;
    if (lanes > 4) { say("the mode needs more FDI bandwidth than %s", "four lanes"); return 0; }

    /* the PCH reference clock: non-spread source on (ironlake_init_pch_refclk) */
    u32 dref = rd(PCH_DREF_CONTROL);
    if ((dref & DREF_NONSPREAD_SOURCE_MASK) != DREF_NONSPREAD_SOURCE_ENABLE) {
        wr(PCH_DREF_CONTROL, (dref & ~DREF_NONSPREAD_SOURCE_MASK) | DREF_NONSPREAD_SOURCE_ENABLE);
        (void)rd(PCH_DREF_CONTROL);
        hal_delay_us(200);
    }
    /* intel_prepare_shared_dpll: the dividers of PCH DPLL A */
    u32 fp = (u32)c.n << 16 | (u32)c.m1 << 8 | (u32)c.m2;
    if (c.m < 21 * c.n) fp |= FP_CB_TUNE;
    wr(PCH_FPA0, fp);
    wr(PCH_FPA1, fp);
    u32 dpll = DPLL_VCO_ENABLE | DPLLB_MODE_DAC_SERIAL | DPLL_SDVO_HIGH_SPEED |
               (1u << (c.p1 - 1)) << 16 | (1u << (c.p1 - 1)) | (c.p2 == 5 ? DPLL_DAC_SERIAL_P2_CLOCK_DIV_5 : 0);

    /* intel_set_pipe_timings, PIPESRC: the panel's framebuffer size */
    wr(VSYNCSHIFT_B, 0);
    wr(HTOTAL_B, (u32)(m->hdisplay - 1) | (u32)(m->htotal - 1) << 16);
    wr(HBLANK_B, (u32)(m->hdisplay - 1) | (u32)(m->htotal - 1) << 16);
    wr(HSYNC_B, (u32)(m->hsync_start - 1) | (u32)(m->hsync_end - 1) << 16);
    wr(VTOTAL_B, (u32)(m->vdisplay - 1) | (u32)(m->vtotal - 1) << 16);
    wr(VBLANK_B, (u32)(m->vdisplay - 1) | (u32)(m->vtotal - 1) << 16);
    wr(VSYNC_B, (u32)(m->vsync_start - 1) | (u32)(m->vsync_end - 1) << 16);
    wr(PIPEBSRC, (fbw - 1) << 16 | (fbh - 1));
    /* FDI M/N (intel_link_compute_m_n: 24 bpp over 2.7 GHz lanes) */
    u32 gm, gn, lm, ln;
    compute_m_n((u64)24 * (u64)m->clock, (u64)270000 * (u64)lanes * 8, &gm, &gn);
    compute_m_n((u64)m->clock, 270000, &lm, &ln);
    wr(PIPEB_DATA_M1, TU_SIZE(64) | gm);
    wr(PIPEB_DATA_N1, gn);
    wr(PIPEB_LINK_M1, lm);
    wr(PIPEB_LINK_N1, ln);
    wr(PIPEBCONF, 0);                                                 /* 8 bpc, progressive */
    (void)rd(PIPEBCONF);

    /* the encoder's pre-enable: the port, not yet on (intel_hdmi_prepare) */
    u32 hr = hdmi_reg[H.port];
    u32 hv = SDVO_ENCODING_HDMI | SDVO_PIPE_SEL_CPT(1) | (m->pvsync ? SDVO_VSYNC_ACTIVE_HIGH : 0) | (m->phsync ? SDVO_HSYNC_ACTIVE_HIGH : 0);
    wr(hr, hv);
    (void)rd(hr);

    /* ironlake_fdi_pll_enable */
    u32 r = rd(FDI_RXB_CTL) & ~(FDI_DP_PORT_WIDTH_MASK | (7u << 16));
    r |= FDI_DP_PORT_WIDTH(lanes) | (rd(PIPEBCONF) & PIPECONF_BPC_MASK) << 11;
    wr(FDI_RXB_CTL, r | FDI_RX_PLL_ENABLE);
    (void)rd(FDI_RXB_CTL); hal_delay_us(200);
    wr(FDI_RXB_CTL, rd(FDI_RXB_CTL) | FDI_PCDCLK);
    (void)rd(FDI_RXB_CTL); hal_delay_us(200);
    if (!(rd(FDI_TXB_CTL) & FDI_TX_PLL_ENABLE)) { wr(FDI_TXB_CTL, rd(FDI_TXB_CTL) | FDI_TX_PLL_ENABLE); (void)rd(FDI_TXB_CTL); hal_delay_us(100); }

    /* ironlake_pfit_enable: the framebuffer scaled to the mode, its shape kept */
    int ww = m->hdisplay, wh = (int)((u64)m->hdisplay * fbh / fbw);
    if (wh > m->vdisplay) { wh = m->vdisplay; ww = (int)((u64)m->vdisplay * fbw / fbh); }
    ww &= ~1; wh &= ~1;
    wr(PFB_CTL, PF_ENABLE | PF_FILTER_MED_3x3 | PF_PIPE_SEL_IVB(1));
    wr(PFB_WIN_POS, (u32)((m->hdisplay - ww) / 2) << 16 | (u32)((m->vdisplay - wh) / 2));
    wr(PFB_WIN_SZ, (u32)ww << 16 | (u32)wh);

    /* watermarks: pipe B asks early; the low-power levels off with two pipes (ilk_update_wm) */
    wr(WM3_LP_ILK, rd(WM3_LP_ILK) & ~(1u << 31));
    wr(WM2_LP_ILK, rd(WM2_LP_ILK) & ~(1u << 31));
    wr(WM1_LP_ILK, rd(WM1_LP_ILK) & ~(1u << 31));
    wr(WM0_PIPEB_ILK, 0x7fu << 16 | 0x7fu << 8 | 0x3fu);

    /* intel_enable_pipe */
    wr(PIPEBCONF, rd(PIPEBCONF) | PIPECONF_ENABLE);
    (void)rd(PIPEBCONF);

    /* ironlake_pch_enable */
    u32 sc = rd(SOUTH_CHICKEN1);
    int bif = lanes <= 2;                                              /* ivybridge_update_fdi_bc_bifurcation */
    if (!!(sc & FDI_BC_BIFURCATION_SELECT) != bif) wr(SOUTH_CHICKEN1, bif ? sc | FDI_BC_BIFURCATION_SELECT : sc & ~FDI_BC_BIFURCATION_SELECT);
    wr(FDI_RXB_TUSIZE1, rd(PIPEB_DATA_M1) & TU_SIZE_MASK);
    if (!fdi_train(lanes)) { say("FDI link training %s", "failed"); hdmi_disable(); return 0; }
    u32 sel = rd(PCH_DPLL_SEL);
    sel |= TRANS_DPLL_ENABLE(1);
    sel &= ~TRANS_DPLLB_SEL(1);                                        /* transcoder B from PCH DPLL A */
    wr(PCH_DPLL_SEL, sel);
    wr(PCH_DPLL_A, dpll);                                              /* ibx_pch_dpll_enable */
    (void)rd(PCH_DPLL_A); hal_delay_us(150);
    wr(PCH_DPLL_A, dpll);
    (void)rd(PCH_DPLL_A); hal_delay_us(200);
    wr(PCH_TRANS_HTOTAL_B, rd(HTOTAL_B));                              /* ironlake_pch_transcoder_set_timings */
    wr(PCH_TRANS_HBLANK_B, rd(HBLANK_B));
    wr(PCH_TRANS_HSYNC_B, rd(HSYNC_B));
    wr(PCH_TRANS_VTOTAL_B, rd(VTOTAL_B));
    wr(PCH_TRANS_VBLANK_B, rd(VBLANK_B));
    wr(PCH_TRANS_VSYNC_B, rd(VSYNC_B));
    wr(PCH_TRANS_VSYNCSHIFT_B, rd(VSYNCSHIFT_B));
    /* intel_fdi_normal_train */
    wr(FDI_TXB_CTL, (rd(FDI_TXB_CTL) & ~FDI_LINK_TRAIN_NONE_IVB) | FDI_LINK_TRAIN_NONE_IVB | FDI_TX_ENHANCE_FRAME_ENABLE);
    wr(FDI_RXB_CTL, (rd(FDI_RXB_CTL) & ~FDI_LINK_TRAIN_PATTERN_MASK_CPT) | FDI_LINK_TRAIN_NORMAL_CPT | FDI_RX_ENHANCE_FRAME_ENABLE);
    (void)rd(FDI_RXB_CTL); hal_delay_us(1000);
    wr(FDI_RXB_CTL, rd(FDI_RXB_CTL) | FDI_FS_ERRC_ENABLE | FDI_FE_ERRC_ENABLE);
    /* ironlake_enable_pch_transcoder */
    wr(TRANSB_CHICKEN2, rd(TRANSB_CHICKEN2) | TRANS_CHICKEN2_TIMING_OVERRIDE);
    wr(PCH_TRANSBCONF, (rd(PCH_TRANSBCONF) & ~(3u << 21)) | TRANS_ENABLE);
    if (!wait_reg(PCH_TRANSBCONF, TRANS_STATE_ENABLE, TRANS_STATE_ENABLE, 100)) { say("the PCH transcoder did not %s", "start"); hdmi_disable(); return 0; }

    /* cpt_enable_hdmi */
    wr(hr, rd(hr) | SDVO_ENABLE);
    (void)rd(hr);

    /* plane B: the panel's framebuffer, as plane A shows it */
    u32 acntr = rd(DSPACNTR);
    wr(DSPBCNTR, DISPLAY_PLANE_ENABLE | DISPPLANE_BGRX888 | DISPPLANE_TRICKLE_FEED_DISABLE | (acntr & DISPPLANE_TILED));
    wr(DSPBSTRIDE, rd(DSPASTRIDE));
    wr(DSPBADDR, 0);
    wr(DSPBTILEOFF, 0);
    wr(DSPBSURF, rd(DSPASURF));
    (void)rd(DSPBSURF);
    H.up = 1;
    fmt(H.status, sizeof H.status, "%s on HDMI %c: %d x %d at %d.%03d MHz (%d FDI lane%s), mirroring the screen", H.monitor, 'A' + H.port,
        m->hdisplay, m->vdisplay, m->clock / 1000, m->clock % 1000, lanes, lanes == 1 ? "" : "s");
    LOG("hdmi: %s", H.status);
    return 1;
}

static void hdmi_thread(void *arg) {
    u8 e[128];
    for (;;) {
        if (H.up) {
            if (read_edid(H.port, e)) { if (++H.misses >= 2) { LOG("hdmi: %s unplugged", H.monitor); hdmi_disable(); strlcpy(H.status, "No monitor on HDMI", sizeof H.status); } }
            else H.misses = 0;
        } else {
            for (int p = 1; p <= 3; p++) {
                if (read_edid(p, e)) continue;
                H.port = p;
                H.misses = 0;
                pick_mode(e);
                LOG("hdmi: %s on port %c, %d x %d at %d kHz", H.monitor, 'A' + p, H.m.hdisplay, H.m.vdisplay, H.m.clock);
                if (!hdmi_enable()) thread_sleep_ms(8000);           /* do not retry at once */
                break;
            }
        }
        thread_sleep_ms(2000);
    }
}

void ivb_display_start(void) {
    if (!g.pci || !g.mmio) { strlcpy(H.status, "No Ivy Bridge display engine", sizeof H.status); return; }
    if ((rd(PIPEBCONF) & PIPECONF_ENABLE)) { strlcpy(H.status, "Pipe B is in use by the firmware: HDMI left alone", sizeof H.status); return; }
    strlcpy(H.status, "No monitor on HDMI", sizeof H.status);
    thread_create("hdmi", hdmi_thread, NULL, 0);
}
const char *ivb_display_status(void) { return H.status[0] ? H.status : "not started"; }
int ivb_display_connected(void) { return H.up; }
const char *ivb_display_monitor(void) { return H.monitor; }

#else
int ivb_matches(const pci_dev_t *d) { (void)d; return 0; }
int ivb_probe(pci_dev_t *d) { (void)d; return 0; }
int ivb_present_supported(void) { return 0; }
int ivb_active(void) { return 0; }
const char *ivb_status(void) { return "needs the 64-bit native kernel"; }
void ivb_set_enabled(int on) { (void)on; }
void ivb_stats(u32 *frames, u32 *avg_us) { *frames = 0; *avg_us = 0; }
int ivb_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h) {
    (void)src; (void)sw; (void)sh; (void)stride; (void)rot; (void)x; (void)y; (void)w; (void)h; return 0;
}
void ivb_display_start(void) {}
const char *ivb_display_status(void) { return "needs the 64-bit native kernel"; }
int ivb_display_connected(void) { return 0; }
const char *ivb_display_monitor(void) { return ""; }
#endif
