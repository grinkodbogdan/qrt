/* Host test for src/drivers/i915/display.c, the external DisplayPort driver.
 * The display engine is simulated well enough to run the whole hot-plug path:
 * the IOSF sideband (PUNIT power wells, DPIO registers), the PLL lock bit,
 * PIPECONF's active bit and the AUX channel.  A DP sink behind it answers
 * native DPCD reads and writes and I2C-over-AUX EDID reads, and moves its
 * lane status along as the source switches training patterns.  Checked:
 *   - the AUX message format (header, big-endian data registers, reply size);
 *   - the EDID's preferred mode and the link chosen for it (Linux's order:
 *     lowest rate, then fewest lanes), the DPCD link configuration written;
 *   - the transcoder timings and the data/link M/N values;
 *   - clock recovery adjusting the voltage swing when the sink asks for it;
 *   - the plane pointing at the scanout buffer, and mirroring a canvas into it
 *     (scaled to fit, centred);
 *   - unplugging: the pipe, port and PLL are switched off.
 * Build/run: make check */
#include <stdio.h>
#include <stdlib.h>

#define DISPLAY_TEST 1
#include "../src/drivers/i915/display.c"

/* ---- the kernel environment ---------------------------------------------------------- */
kernel_t k;
static u64 now;
u64 k_now_us(void) { return now += 5; }
u64 k_now_ms(void) { return (now += 5) / 1000; }
void hal_delay_us(u32 us) { now += us; }
void thread_sleep_ms(u64 ms) { now += ms * 1000; }
thread_t *thread_create(const char *n, void (*fn)(void *), void *a, u64 cr3) { return NULL; }
u32 hal_setting_get(const c16 *n, u32 def) { return def; }
void hal_setting_set(const c16 *n, u32 v) { }
void *hal_dma_alloc(usize n) { return aligned_alloc(4096, (n + 4095) & ~(usize)4095); }
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
static int verbose;
void klog(const char *f, ...) { if (!verbose) return; va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap); printf("\n"); }
void shell_redraw(void) {}
volatile u8 *gpu_regs(void) { return (volatile u8 *)1; }
static u32 mapped_gtt; static u64 mapped_phys;
void gpu_gtt_map(u32 off, u64 phys, usize pages) { mapped_gtt = off; mapped_phys = phys; }
u32 gpu_scanout_gtt(usize bytes) { return 0x01000000; }
int gpu_present_scaled(const u32 *src, int sw, int sh, int stride, u32 dst_gtt, int dw, int dh, int dpitch,
                       int x0, int y0, float scale, int rx, int ry, int rw, int rh) { return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the simulated hardware ----------------------------------------------------------- */
static u32 mmio[0x200000 / 4];
static u32 punit[256], dpio[2][0x3000 / 4 + 0x1000];
static u8 dpcd[0x1000], edid[256];
static int edid_ptr, sink_on = 1, aux_msgs, cr_after = 2, tp_writes;
static u32 last_pipeconf;

static u32 dpio_idx(u32 a) { return a >= 0x8000 ? 0x3000 / 4 + (a - 0x8000) / 4 : a / 4; }

static void sink_write(u32 a, u8 v) {
    dpcd[a] = v;
    if (a == 0x102) {
        int tp = v & 3;
        tp_writes++;
        if (tp == 1 && --cr_after > 0) {                      /* first try: ask for more swing */
            dpcd[0x202] = dpcd[0x203] = 0;
            dpcd[0x206] = dpcd[0x207] = 0x11;                 /* voltage swing 1 on every lane */
        } else if (tp == 1) { dpcd[0x202] = dpcd[0x203] = 0x11; dpcd[0x204] = 0; }
        else if (tp == 2) { dpcd[0x202] = dpcd[0x203] = 0x77; dpcd[0x204] = 1; }
    }
    if (a >= 0x103 && a <= 0x106 && dpcd[0x102] == 0x21 && dpcd[a] == 1)
        dpcd[0x202] = dpcd[0x203] = 0x11;                     /* swing 1 set: clock recovered */
}

static void aux(u32 ctl, u32 v) {
    int n = (int)((v >> 20) & 0x1f);
    u8 m[20], r[20];
    int rn = 1;
    for (int i = 0; i < n; i++) m[i] = (u8)(mmio[(ctl + 4 + (i & ~3)) / 4] >> (24 - 8 * (i & 3)));
    aux_msgs++;
    u32 port = (ctl - (DB + 0x64010)) / 0x100;
    if (!sink_on || port != 2) { mmio[ctl / 4] = AUX_DONE | AUX_TIMEOUT_ERR; return; }   /* the sink is on port C */
    int cmd = m[0] >> 4;
    u32 a = (u32)(m[0] & 0xf) << 16 | (u32)m[1] << 8 | m[2];
    r[0] = 0;
    if (cmd == 9) { int len = m[3] + 1; memcpy(r + 1, dpcd + a, (usize)len); rn = 1 + len; }
    else if (cmd == 8) { for (int i = 0; i <= m[3]; i++) sink_write(a + (u32)i, m[4 + i]); }
    else if ((cmd & 0xb) == 0 && n > 4) edid_ptr = m[4];       /* I2C write: the offset */
    else if ((cmd & 0xb) == 1 && n == 4) { int len = m[3] + 1; memcpy(r + 1, edid + edid_ptr, (usize)len); edid_ptr += len; rn = 1 + len; }
    for (int i = 0; i < rn; i += 4) {
        u32 w = 0;
        for (int b = 0; b < 4 && i + b < rn; b++) w |= (u32)r[i + b] << (24 - 8 * b);
        mmio[(ctl + 4 + i) / 4] = w;
    }
    mmio[ctl / 4] = AUX_DONE | (u32)rn << 20;
}

static u32 rd(u32 r) { return mmio[r / 4]; }
static void wr(u32 r, u32 v) {
    if (r == VLV_IOSF_DOORBELL) {                             /* a sideband message */
        u32 op = (v >> 16) & 0xff, port = (v >> 8) & 0xff, a = mmio[VLV_IOSF_ADDR / 4], d = mmio[VLV_IOSF_DATA / 4];
        if (port == PORT_PUNIT) {
            if (op == SB_CRWRDA_NP) { punit[a & 0xff] = d; if (a == PWRGT_CTRL) punit[PWRGT_STATUS] = d; }
            else mmio[VLV_IOSF_DATA / 4] = punit[a & 0xff];
        } else if (port == PORT_CCK) mmio[VLV_IOSF_DATA / 4] = a == 0x8 ? 1 : a == 0x6c ? 15 : 0;   /* 1600 MHz / 16 */
        else {
            int phy = port == PORT_DPIO1;
            if (op == SB_MWR_NP) dpio[phy][dpio_idx(a)] = d; else mmio[VLV_IOSF_DATA / 4] = dpio[phy][dpio_idx(a)];
        }
        return;                                               /* doorbell busy bit stays clear */
    }
    if ((r & 0xff) == 0x10 && r >= DB + 0x64110 && r <= DB + 0x64310 && (v & AUX_SEND_BUSY)) { aux(r, v); return; }
    if ((r & 0xff) == 0x10 && r >= DB + 0x64110 && r <= DB + 0x64310) { mmio[r / 4] &= ~(v & (AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR)); return; }
    mmio[r / 4] = v;
    if (r == DPLL_B || r == DPLL_C) mmio[r / 4] = (v & DPLL_VCO_ENABLE) ? v | DPLL_LOCK_VLV : v & ~DPLL_LOCK_VLV;
    if (r == DB + 0x71008 || r == DB + 0x74008) { last_pipeconf = v; mmio[r / 4] = (v & (1u << 31)) ? v | 1u << 30 : v & ~(1u << 30); }
}

/* an EDID 1.3 block: "DOCKMON", preferred 1920x1080@60 (CEA-861 timing) */
static void make_edid(void) {
    static const u8 hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    memcpy(edid, hdr, 8);
    edid[0x12] = 1; edid[0x13] = 3;
    u8 *d = edid + 54;
    d[0] = 0x02; d[1] = 0x3a;                                 /* 148.50 MHz */
    d[2] = 0x80; d[3] = 0x18; d[4] = 0x71;                    /* 1920 active, 280 blank */
    d[5] = 0x38; d[6] = 0x2d; d[7] = 0x40;                    /* 1080 active, 45 blank */
    d[8] = 0x58; d[9] = 0x2c; d[10] = 0x45; d[11] = 0x00;     /* hsync 88+44, vsync 4+5 */
    d[17] = 0x1e;                                             /* digital separate, +h +v */
    u8 *n = edid + 72;
    n[3] = 0xfc; memcpy(n + 5, "DOCKMON\n     ", 13);
    u8 sum = 0;
    for (int i = 0; i < 127; i++) sum += edid[i];
    edid[127] = (u8)-sum;
}

int main(int argc, char **argv) {
    verbose = argc > 1;
    k.native = 1; k.is_venue = 1;
    D.R = (volatile u8 *)mmio;
    mmio[DISPLAY_PHY_STATUS / 4] = 3u << 30;                  /* both PHYs report power good */
    punit[PWRGT_STATUS] = 0xffffffff;                         /* every well off */
    punit[PWRGT_CTRL] = 0xffffffff;
    dpcd[0] = 0x12; dpcd[1] = 0x0a; dpcd[2] = 0x84; dpcd[0xe] = 0;   /* DP 1.2, 2.7 Gbps, 4 lanes, enhanced framing */
    make_edid();

    /* nothing answers on port B, the sink answers on port C */
    aux_div = 100;
    phy_ctl_init();
    CHECK(!probe_port(1), "port B has no sink");
    CHECK(probe_port(2), "port C's sink was not found");
    CHECK(well_on(WELL_CMN_BC), "the B/C PHY power well is off");
    CHECK(D.phy_ctl & LANE_RESET_DEASSERT(0), "PHY0 common lane reset not released");
    connect(2);
    CHECK(D.state == D_ON, "connect failed: %s", D.status);
    CHECK(!strcmp(D.monitor, "DOCKMON"), "monitor name '%s'", D.monitor);
    CHECK(D.m.hd == 1920 && D.m.vd == 1080 && D.m.clock == 148500 && D.m.ht == 2200 && D.m.vt == 1125,
          "mode %dx%d %d kHz total %dx%d", D.m.hd, D.m.vd, D.m.clock, D.m.ht, D.m.vt);
    CHECK(D.m.hss == 2008 && D.m.hse == 2052 && D.m.vss == 1084 && D.m.vse == 1089, "sync %d-%d %d-%d", D.m.hss, D.m.hse, D.m.vss, D.m.vse);
    CHECK(D.rate == 162000 && D.lanes == 4, "link %d x%d (want 1.62 Gbps x4)", D.rate, D.lanes);
    CHECK(dpcd[0x100] == 0x06 && dpcd[0x101] == 0x84, "DPCD link config %02x %02x", dpcd[0x100], dpcd[0x101]);
    CHECK(dpcd[0x102] == 0, "training pattern left on (%02x)", dpcd[0x102]);
    CHECK(dpcd[0x600] == 1, "sink not in D0");
    CHECK(dpcd[0x103] == 1, "clock recovery did not raise the swing (%02x)", dpcd[0x103]);
    CHECK(D.pipe == 1, "port C must use pipe B");
    /* the transcoder */
    CHECK(rd(DB + 0x61000) == (1919u | 2199u << 16), "HTOTAL %08x", rd(DB + 0x61000));
    CHECK(rd(DB + 0x61008) == (2007u | 2051u << 16), "HSYNC %08x", rd(DB + 0x61008));
    CHECK(rd(DB + 0x6100c) == (1079u | 1124u << 16), "VTOTAL %08x", rd(DB + 0x6100c));
    CHECK(rd(DB + 0x6101c) == (1919u << 16 | 1079u), "PIPESRC %08x", rd(DB + 0x6101c));
    /* M/N: data 24*148500 / (162000*4*8), link 148500 / 162000; N a power of two (Linux) */
    u32 dm = rd(DB + 0x61030) & 0xffffff, dn = rd(DB + 0x61034), lm = rd(DB + 0x61040), ln = rd(DB + 0x61044);
    CHECK(dn == 0x800000 && dm == (u32)((u64)24 * 148500 * 0x800000 / (162000 * 32)), "data M/N %x/%x", dm, dn);
    CHECK((rd(DB + 0x61030) >> 25) == 63, "TU size");
    CHECK(ln == 0x40000 && lm == (u32)((u64)148500 * 0x40000 / 162000), "link M/N %x/%x", lm, ln);
    /* the port, PLL, pipe, plane */
    u32 port = rd(DB + 0x64200);
    CHECK((port & DP_PORT_EN) && ((port >> 16) & 3) == 1 && ((port >> 19) & 7) == 3 && (port & DP_ENHANCED_FRAMING) &&
          (port & DP_LINK_TRAIN_MASK_CHV) == DP_LINK_TRAIN_OFF && (port & (DP_SYNC_HS_HIGH | DP_SYNC_VS_HIGH)) == (DP_SYNC_HS_HIGH | DP_SYNC_VS_HIGH),
          "DP_C %08x", port);
    CHECK(rd(DPLL_B) & DPLL_VCO_ENABLE, "PLL B off");
    CHECK(dpio[0][dpio_idx(PLL_DW0(1))] == (0x819999au >> 22) && dpio[0][dpio_idx(PLL_DW2(1))] == (0x819999au & 0x3fffff),
          "PLL m2 %x.%x", dpio[0][dpio_idx(PLL_DW0(1))], dpio[0][dpio_idx(PLL_DW2(1))]);
    CHECK(dpio[0][dpio_idx(CMN_DW13(1))] == (5u << 21 | 4u << 13 | 2u << 8 | 1u << 4), "PLL p1/p2 %x", dpio[0][dpio_idx(CMN_DW13(1))]);
    CHECK(last_pipeconf == 1u << 31 && (rd(DB + 0x71008) & 1u << 30), "pipe B not enabled");
    CHECK(rd(DB + 0x71180) == ((1u << 31) | (6u << 26)), "DSPCNTR %08x", rd(DB + 0x71180));
    CHECK(rd(DB + 0x7119c) == 0x01000000 && mapped_gtt == 0x01000000 && mapped_phys == (u64)(usize)D.fb, "surface");
    CHECK(rd(DB + 0x71188) == 1920 * 4, "stride %u", rd(DB + 0x71188));
    CHECK(rd(DB + 0x61a0c) == (1079u << 16 | 1919u), "PRIMSIZE %08x", rd(DB + 0x61a0c));

    /* mirror a 600x960 canvas (the tablet's logical screen at 2x): fits 1080 tall at 1.125, centred */
    int cw = 600, ch = 960;
    u32 *canvas = malloc((usize)cw * ch * 4);
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) canvas[y * cw + x] = (u32)(x << 12 | y);
    display_mirror(canvas, cw, ch, cw, 0, 0, cw, ch);
    float s = 1080.0f / 960;
    int x0 = (1920 - (int)(cw * s)) / 2;
    CHECK(D.x0 == x0 && D.y0 == 0, "placement %d,%d", D.x0, D.y0);
    u32 *fb = D.fb;
    CHECK(fb[0] == 0 && fb[x0 - 1] == 0, "letterbox not black");
    CHECK(fb[x0] == canvas[0], "top-left pixel %08x", fb[x0]);
    int X = x0 + 300, Y = 540;
    CHECK(fb[Y * 1920 + X] == canvas[(int)(Y / s) * cw + (int)(300 / s)], "middle pixel");
    /* a small damaged rectangle only touches its own area */
    canvas[100 * cw + 100] = 0xffffffff;
    display_mirror(canvas, cw, ch, cw, 100, 100, 1, 1);
    CHECK(fb[(int)(100 * s + 0.5f) * 1920 + x0 + (int)(100 * s + 0.5f)] == 0xffffffff, "damaged pixel not mirrored");
    CHECK(D.frames == 2, "frames %u", D.frames);

    /* unplug */
    sink_on = 0;
    CHECK(!sink_present(), "the sink still answers");
    D.busy = 1; disable(); D.busy = 0; D.state = D_SEARCHING;
    CHECK(!(rd(DB + 0x71008) & (3u << 30)), "pipe still on");
    CHECK(!(rd(DB + 0x71180) & (1u << 31)), "plane still on");
    CHECK(!(rd(DB + 0x64200) & DP_PORT_EN), "port still on");
    CHECK(!(rd(DPLL_B) & DPLL_VCO_ENABLE), "PLL still on");
    display_mirror(canvas, cw, ch, cw, 0, 0, cw, ch);
    CHECK(D.frames == 2, "mirrored while off");

    /* a 2560x1440@60 (241.5 MHz) monitor whose sink has two lanes */
    dpcd[2] = 0x82;
    D.m = (dmode_t){ 0 };
    sink_on = 1; cr_after = 1;
    edid[54] = 0x56; edid[55] = 0x5e;                         /* 241.50 MHz */
    edid[56] = 0x00; edid[57] = 0xa0; edid[58] = 0xa0;        /* 2560 active, 160 blank */
    edid[59] = 0xa0; edid[60] = 0x29; edid[61] = 0x50;        /* 1440 active, 41 blank */
    edid[62] = 0x30; edid[63] = 0x20; edid[64] = 0x35; edid[65] = 0x00;
    CHECK(probe_port(2), "port C's sink was not found again");
    connect(2);
    /* 241.5 MHz x 24 bits does not fit 2 x 2.7 Gbps: the 1080p fallback, which needs 2.7 Gbps on 2 lanes */
    CHECK(D.state == D_ON && D.m.hd == 1920 && D.m.vd == 1080, "fallback mode %dx%d (%s)", D.m.hd, D.m.vd, D.status);
    CHECK(D.rate == 270000 && D.lanes == 2, "link %d x%d (want 2.7 Gbps x2)", D.rate, D.lanes);
    CHECK(dpio[0][dpio_idx(PLL_DW0(1))] == (0x6c00000u >> 22) && dpio[0][dpio_idx(PLL_DW2(1))] == 0, "2.7G PLL m2");
    printf("  %s\n", D.status);

    if (fails) { printf("test_display: %d failure(s)\n", fails); return 1; }
    printf("test_display: DP detection, EDID, link training, modeset, mirroring and unplug pass\n");
    return 0;
}
