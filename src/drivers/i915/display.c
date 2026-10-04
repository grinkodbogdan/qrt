/*
 * display.c - external monitors on Cherry Trail's DisplayPort outputs.
 *
 * The Venue 8 Pro 5855's USB-C port switches to DisplayPort Alternate Mode in
 * hardware (its DSDT has no Type-C or PD controller for the OS to drive), so
 * a USB-C dock's monitor appears as a plain DP sink on one of the display
 * engine's ports B, C or D.  The internal panel is MIPI DSI on pipe A and is
 * never touched here.  Ports B and C are driven by pipe B, port D by pipe C
 * (on Cherryview port D can only use pipe C).
 *
 * The sequence follows Linux's i915 (v4.9) for Cherryview, step by step:
 *   intel_runtime_pm.c   the DPIO PHY common-lane power wells (PUNIT), PHY control
 *   intel_sideband.c     IOSF sideband access to the PUNIT, CCK and DPIO PHYs
 *   intel_dp.c           AUX channel, DPCD, port register, signal levels
 *   intel_dp_link_training.c  clock recovery and channel equalization
 *   intel_dpio_phy.c     lane reset, clock distribution, stagger, swing/de-emphasis
 *   intel_display.c      chv_prepare_pll / chv_enable_pll (fixed DP dividers),
 *                        pipe timings, link M/N, pipe and primary plane
 *   intel_pm.c           the FIFO split and watermarks for the new pipe
 * A kernel thread polls the ports once a second (hot-plug), reads the
 * monitor's EDID, picks a mode it can carry, trains the link and scans out a
 * buffer of its own.  The shell's picture is scaled into that buffer - by
 * the GPU when it is up, by the CPU otherwise - so the monitor mirrors the
 * tablet.  Every step has a time limit and a failure leaves the port off,
 * with the step named in the status line.
 */
#include "display.h"
#include "gpu.h"
#include "ivb.h"

void shell_redraw(void);

#if defined(__x86_64__)
#include "../../arch/x64/sched.h"

#define DB 0x180000u                         /* VLV_DISPLAY_BASE */

/* ---- registers ------------------------------------------------------------------ */
#define VLV_IOSF_DOORBELL  (DB + 0x2100)
#define VLV_IOSF_DATA      (DB + 0x2104)
#define VLV_IOSF_ADDR      (DB + 0x2108)
#define DPLL_A             (DB + 0x6014)
#define DPLL_B             (DB + 0x6018)
#define DPLL_C             (DB + 0x6030)
#define DPLL_MD_B          (DB + 0x6020)
#define DPIO_PHY_STATUS    (DB + 0x6240)
#define FW_BLC_SELF_VLV    (DB + 0x6500)
#define DISPLAY_PHY_CONTROL (DB + 0x60100)
#define DISPLAY_PHY_STATUS (DB + 0x60104)
#define PORT_HOTPLUG_STAT  (DB + 0x61114)
#define DSPARB             (DB + 0x70030)
#define DSPFW1             (DB + 0x70034)
#define DSPARB2            (DB + 0x70060)
#define DSPHOWM            (DB + 0x70064)
#define DSPARB3            (DB + 0x7006c)
#define DSPFW9_CHV         (DB + 0x7007c)
#define CBR4_VLV           (DB + 0x70450)

#define DPLL_VCO_ENABLE            (1u << 31)
#define DPLL_REF_CLK_ENABLE_VLV    (1u << 29)
#define DPLL_VGA_MODE_DIS          (1u << 28)
#define DPLL_LOCK_VLV              (1u << 15)
#define DPLL_INTEGRATED_CRI_CLK    (1u << 14)
#define DPLL_SSC_REF_CLK_CHV       (1u << 13)

#define DP_PORT_EN                 (1u << 31)
#define DP_LINK_TRAIN_PAT_1        (0u << 28)
#define DP_LINK_TRAIN_PAT_2        (1u << 28)
#define DP_LINK_TRAIN_OFF          (3u << 28)
#define DP_LINK_TRAIN_MASK_CHV     ((3u << 28) | (1u << 14))
#define DP_ENHANCED_FRAMING        (1u << 18)
#define DP_SYNC_VS_HIGH            (1u << 4)
#define DP_SYNC_HS_HIGH            (1u << 3)
#define DP_DETECTED                (1u << 2)

#define AUX_SEND_BUSY              (1u << 31)
#define AUX_DONE                   (1u << 30)
#define AUX_TIMEOUT_ERR            (1u << 28)
#define AUX_RECV_ERR               (1u << 25)

/* DPIO (sideband) registers; ch = DPIO channel, lane = data lane */
#define PORTSEL(ch, a, b)          ((a) + (ch) * ((b) - (a)))
#define PCS01_DW0(ch)   PORTSEL(ch, 0x200, 0x2600)
#define PCS23_DW0(ch)   PORTSEL(ch, 0x400, 0x2800)
#define PCS01_DW1(ch)   PORTSEL(ch, 0x204, 0x2604)
#define PCS23_DW1(ch)   PORTSEL(ch, 0x404, 0x2804)
#define PCS01_DW8(ch)   PORTSEL(ch, 0x220, 0x2620)
#define PCS23_DW8(ch)   PORTSEL(ch, 0x420, 0x2820)
#define PCS01_DW9(ch)   PORTSEL(ch, 0x224, 0x2624)
#define PCS23_DW9(ch)   PORTSEL(ch, 0x424, 0x2824)
#define PCS01_DW10(ch)  PORTSEL(ch, 0x228, 0x2628)
#define PCS23_DW10(ch)  PORTSEL(ch, 0x428, 0x2828)
#define PCS01_DW11(ch)  PORTSEL(ch, 0x22c, 0x262c)
#define PCS23_DW11(ch)  PORTSEL(ch, 0x42c, 0x282c)
#define PCS01_DW12(ch)  PORTSEL(ch, 0x230, 0x2630)
#define PCS23_DW12(ch)  PORTSEL(ch, 0x430, 0x2830)
#define TXLANE(ch, lane, off) (((ch) ? 0x2400 : 0) + (lane) * 0x200 + (off))
#define TX_DW2(ch, l)   TXLANE(ch, l, 0x88)
#define TX_DW3(ch, l)   TXLANE(ch, l, 0x8c)
#define TX_DW4(ch, l)   TXLANE(ch, l, 0x90)
#define TX_DW14(ch, l)  TXLANE(ch, l, 0xb8)
#define PLL_DW0(ch)     PORTSEL(ch, 0x8000, 0x8180)
#define PLL_DW1(ch)     PORTSEL(ch, 0x8004, 0x8184)
#define PLL_DW2(ch)     PORTSEL(ch, 0x8008, 0x8188)
#define PLL_DW3(ch)     PORTSEL(ch, 0x800c, 0x818c)
#define PLL_DW6(ch)     PORTSEL(ch, 0x8018, 0x8198)
#define PLL_DW8(ch)     PORTSEL(ch, 0x8020, 0x81a0)
#define PLL_DW9(ch)     PORTSEL(ch, 0x8024, 0x81a4)
#define CMN_DW13(ch)    PORTSEL(ch, 0x8134, 0x8080)
#define CMN_DW14(ch)    PORTSEL(ch, 0x8138, 0x8084)
#define CMN_DW19(ch)    PORTSEL(ch, 0x814c, 0x8098)
#define CMN_DW5_CH0     0x8114
#define CMN_DW1_CH1     0x8084
#define CMN_DW6_CH1     0x8098
#define CMN_DW28        0x8170
#define CMN_DW30        0x8178

/* ---- state ---------------------------------------------------------------------- */
enum { D_OFF, D_SEARCHING, D_ON, D_FAILED, D_DISABLED };

typedef struct { int clock, hd, hss, hse, ht, vd, vss, vse, vt, hpos, vpos; } dmode_t;

static struct {
    volatile u8 *R;
    volatile int state;
    char status[120];
    int port;                  /* 1 = B, 2 = C, 3 = D */
    int pipe;                  /* 1 = B, 2 = C */
    int lanes, rate;           /* link: lane count, symbol clock in kHz (162000 / 270000) */
    u8 dpcd[16];
    char monitor[14];
    dmode_t m;
    u32 dp;                    /* the port register value */
    u8 train[4];
    u32 phy_ctl;               /* DISPLAY_PHY_CONTROL shadow (Linux: never read it) */
    int cl2_release;
    u32 *fb;                   /* the scanout buffer */
    u32 fb_gtt, pitch;
    usize fb_bytes;
    /* mirroring */
    int last_w, last_h, x0, y0;
    float scale;
    volatile int busy;         /* the thread is changing the mode: no mirroring */
    int virt;                  /* display_virtual(): a test monitor in memory */
    u32 frames;
} D;

#ifndef DISPLAY_TEST                                  /* tests/test_display.c simulates the hardware */
static u32 rd(u32 r) { return *(volatile u32 *)(D.R + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(D.R + r) = v; }
#define SB_LOCK   u64 fl = irq_save()
#define SB_UNLOCK irq_restore(fl)
#else
static u32 rd(u32 r);
static void wr(u32 r, u32 v);
#define SB_LOCK   (void)0
#define SB_UNLOCK (void)0
#endif
static int wait_reg(u32 r, u32 mask, u32 want, u32 ms) {
    u64 end = k_now_us() + (u64)ms * 1000;
    while ((rd(r) & mask) != want) { if (k_now_us() > end) return 0; }
    return 1;
}
static void msleep(u32 ms) { thread_sleep_ms(ms); }
static void say(const char *f, const char *a) { fmt(D.status, sizeof D.status, f, a); klog("display: %s", D.status); }

/* ---- IOSF sideband (intel_sideband.c) ---------------------------------------------- */
#define SB_MRD_NP    0x00
#define SB_MWR_NP    0x01
#define SB_CRRDDA_NP 0x06
#define SB_CRWRDA_NP 0x07
#define PORT_PUNIT   0x04
#define PORT_CCK     0x14
#define PORT_DPIO0   0x12
#define PORT_DPIO1   0x1a

static int sb_rw(u32 port, u32 op, u32 addr, u32 *val) {
    int is_read = op == SB_MRD_NP || op == SB_CRRDDA_NP;
    u32 cmd = (0u << 24) | (op << 16) | (port << 8) | (0xfu << 4);
    SB_LOCK;                                           /* one user of the sideband at a time */
    int ok = wait_reg(VLV_IOSF_DOORBELL, 1, 0, 5);
    if (ok) {
        wr(VLV_IOSF_ADDR, addr);
        if (!is_read) wr(VLV_IOSF_DATA, *val);
        wr(VLV_IOSF_DOORBELL, cmd);
        ok = wait_reg(VLV_IOSF_DOORBELL, 1, 0, 5);
        if (ok && is_read) *val = rd(VLV_IOSF_DATA);
        wr(VLV_IOSF_DATA, 0);
    }
    SB_UNLOCK;
    return ok;
}
static u32 punit_rd(u32 a) { u32 v = 0; sb_rw(PORT_PUNIT, SB_CRRDDA_NP, a, &v); return v; }
static void punit_wr(u32 a, u32 v) { sb_rw(PORT_PUNIT, SB_CRWRDA_NP, a, &v); }
static u32 cck_rd(u32 a) { u32 v = 0; sb_rw(PORT_CCK, SB_CRRDDA_NP, a, &v); return v; }
static u32 dpio_rd(int phy, u32 a) { u32 v = 0; sb_rw(phy ? PORT_DPIO1 : PORT_DPIO0, SB_MRD_NP, a, &v); return v; }
static void dpio_wr(int phy, u32 a, u32 v) { sb_rw(phy ? PORT_DPIO1 : PORT_DPIO0, SB_MWR_NP, a, &v); }

/* ---- PHY power (intel_runtime_pm.c) ------------------------------------------------- */
#define PWRGT_CTRL   0x60
#define PWRGT_STATUS 0x61
#define WELL_CMN_BC  5
#define WELL_CMN_D   12
#define OVRD_EN(phy, ch)        (1u << (2 * (phy) + (ch) + 27))
#define OVRD(mask, phy, ch)     ((u32)(mask) << (8 * (phy) + 4 * (ch) + 11))
#define LDO_DELAY(phy)          (2u << (2 * (phy) + 23))
#define POWER_MODE_PSR(phy, ch) (7u << (6 * (phy) + 3 * (ch) + 2))
#define LANE_RESET_DEASSERT(phy) (1u << (phy))

static int well_on(int id) { return (punit_rd(PWRGT_STATUS) & (3u << (id * 2))) == 0; }

static void phy_ctl_init(void) {
    D.phy_ctl = LDO_DELAY(0) | LDO_DELAY(1) | POWER_MODE_PSR(0, 0) | POWER_MODE_PSR(0, 1) | POWER_MODE_PSR(1, 0);
    if (well_on(WELL_CMN_BC)) {
        u32 st = rd(DPLL_A), mask = st & 0xf;
        if (mask == 0xf) mask = 0; else D.phy_ctl |= OVRD_EN(0, 0);
        D.phy_ctl |= OVRD(mask, 0, 0);
        mask = (st >> 4) & 0xf;
        if (mask == 0xf) mask = 0; else D.phy_ctl |= OVRD_EN(0, 1);
        D.phy_ctl |= OVRD(mask, 0, 1) | LANE_RESET_DEASSERT(0);
    }
    if (well_on(WELL_CMN_D)) {
        u32 mask = rd(DPIO_PHY_STATUS) & 0xf;
        if (mask == 0xf) mask = 0; else D.phy_ctl |= OVRD_EN(1, 0);
        D.phy_ctl |= OVRD(mask, 1, 0) | LANE_RESET_DEASSERT(1);
    }
    wr(DISPLAY_PHY_CONTROL, D.phy_ctl);
}

/* chv_dpio_cmn_power_well_enable */
static int cmn_enable(int phy) {
    int id = phy ? WELL_CMN_D : WELL_CMN_BC;
    if (well_on(id) && (D.phy_ctl & LANE_RESET_DEASSERT(phy))) return 1;
    hal_delay_us(1);
    u32 ctrl = punit_rd(PWRGT_CTRL) & ~(3u << (id * 2));               /* PWR_ON = 0 */
    punit_wr(PWRGT_CTRL, ctrl);
    u64 end = k_now_ms() + 100;
    while (!well_on(id)) if (k_now_ms() > end) return 0;
    if (!wait_reg(DISPLAY_PHY_STATUS, phy ? 1u << 30 : 1u << 31, phy ? 1u << 30 : 1u << 31, 2)) return 0;
    dpio_wr(phy, CMN_DW28, dpio_rd(phy, CMN_DW28) | (1u << 22) | (1u << 23) | 3u);   /* dynamic power down */
    if (!phy) dpio_wr(phy, CMN_DW6_CH1, dpio_rd(phy, CMN_DW6_CH1) | (1u << 28));
    else dpio_wr(phy, CMN_DW30, dpio_rd(phy, CMN_DW30) | (1u << 6));
    D.phy_ctl |= LANE_RESET_DEASSERT(phy);
    wr(DISPLAY_PHY_CONTROL, D.phy_ctl);
    return 1;
}

static int powergate_ch(int phy, int ch, int override) {      /* returns the previous override */
    int was = (D.phy_ctl & OVRD_EN(phy, ch)) != 0;
    if (override) D.phy_ctl |= OVRD_EN(phy, ch); else D.phy_ctl &= ~OVRD_EN(phy, ch);
    wr(DISPLAY_PHY_CONTROL, D.phy_ctl);
    return was;
}

static void powergate_lanes(int phy, int ch, int override, u32 mask) {
    D.phy_ctl &= ~OVRD(0xf, phy, ch);
    D.phy_ctl |= OVRD(mask, phy, ch);
    if (override) D.phy_ctl |= OVRD_EN(phy, ch); else D.phy_ctl &= ~OVRD_EN(phy, ch);
    wr(DISPLAY_PHY_CONTROL, D.phy_ctl);
}

/* the port's PHY and data-lane channel; the pipe's PHY and PLL channel */
static int port_phy(void) { return D.port == 3; }
static int port_ch(void) { return D.port == 2; }
static int pipe_phy(void) { return D.pipe == 2; }
static int pipe_ch(void) { return D.pipe == 1; }           /* pipe B -> channel 1, pipe C -> 0 */

/* ---- AUX channel (intel_dp_aux_ch) ------------------------------------------------- */
static u32 aux_div;

static u32 aux_ctl(int port) { return DB + 0x64010 + 0x100u * (u32)port; }

static int aux_xfer(int port, const u8 *send, int ns, u8 *recv, int cap) {
    u32 ctl = aux_ctl(port);
    for (int t = 0; t < 3 && (rd(ctl) & AUX_SEND_BUSY); t++) msleep(1);
    if (rd(ctl) & AUX_SEND_BUSY) return -16;
    u32 st = 0;
    for (int tr = 0; tr < 5; tr++) {
        for (int i = 0; i < ns; i += 4) {
            u32 v = 0;
            for (int b = 0; b < 4 && i + b < ns; b++) v |= (u32)send[i + b] << (24 - 8 * b);
            wr(ctl + 4 + (u32)i, v);
        }
        wr(ctl, AUX_SEND_BUSY | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR | ((u32)ns << 20) | (5u << 16) | aux_div);
        u64 end = k_now_us() + 10000;
        while ((st = rd(ctl)) & AUX_SEND_BUSY) if (k_now_us() > end) break;
        wr(ctl, st | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR);
        if (st & AUX_TIMEOUT_ERR) continue;
        if (st & AUX_RECV_ERR) { hal_delay_us(500); continue; }
        if (st & AUX_DONE) break;
    }
    if (!(st & AUX_DONE)) return -16;
    if (st & (AUX_TIMEOUT_ERR | AUX_RECV_ERR)) return -5;
    int n = (int)((st >> 20) & 0x1f);
    if (n <= 0 || n > 20) return -16;
    for (int i = 0; i < n && i < cap; i += 4) {
        u32 v = rd(ctl + 4 + (u32)i);
        for (int b = 0; b < 4 && i + b < n && i + b < cap; b++) recv[i + b] = (u8)(v >> (24 - 8 * b));
    }
    return n;
}

/* native DPCD access; retries on DEFER */
static int dpcd_read(int port, u32 a, u8 *buf, int len) {
    u8 s[4] = { (u8)(0x90 | (a >> 16)), (u8)(a >> 8), (u8)a, (u8)(len - 1) }, r[20];
    for (int t = 0; t < 8; t++) {
        int n = aux_xfer(port, s, 4, r, sizeof r);
        if (n < 0) return n;
        int code = (r[0] >> 4) & 3;
        if (code == 0) { if (n - 1 < len) return -5; memcpy(buf, r + 1, (usize)len); return len; }
        if (code == 1) return -5;                               /* NACK */
        hal_delay_us(500);                                      /* DEFER */
    }
    return -5;
}

static int dpcd_write(int port, u32 a, const u8 *buf, int len) {
    u8 s[20] = { (u8)(0x80 | (a >> 16)), (u8)(a >> 8), (u8)a, (u8)(len - 1) }, r[4];
    memcpy(s + 4, buf, (usize)len);
    for (int t = 0; t < 8; t++) {
        int n = aux_xfer(port, s, 4 + len, r, sizeof r);
        if (n < 0) return n;
        int code = (r[0] >> 4) & 3;
        if (code == 0) return len;
        if (code == 1) return -5;
        hal_delay_us(500);
    }
    return -5;
}
static int dpcd_wb(int port, u32 a, u8 v) { return dpcd_write(port, a, &v, 1); }

/* I2C over AUX: the EDID at address 0x50 */
static int i2c_aux(int port, u8 cmd, const u8 *w, int wlen, u8 *rbuf, int rlen) {
    u8 s[8], r[20];
    int ns;
    s[0] = (u8)(cmd << 4); s[1] = 0; s[2] = 0x50;
    if (wlen) { s[3] = (u8)(wlen - 1); memcpy(s + 4, w, (usize)wlen); ns = 4 + wlen; }
    else if (rlen) { s[3] = (u8)(rlen - 1); ns = 4; }
    else ns = 3;                                                /* address only */
    for (int t = 0; t < 8; t++) {
        int n = aux_xfer(port, s, ns, r, sizeof r);
        if (n < 0) return n;
        int aux = (r[0] >> 4) & 3, i2c = (r[0] >> 6) & 3;
        if (aux == 0 && i2c == 0) {
            if (rlen) { if (n - 1 < rlen) return -5; memcpy(rbuf, r + 1, (usize)rlen); }
            return 0;
        }
        if (aux == 1 || i2c == 1) return -5;
        hal_delay_us(500);
    }
    return -5;
}

static int read_edid(int port, u8 *edid, int blocks) {
    u8 off = 0;
    if (i2c_aux(port, 0x4, &off, 1, NULL, 0)) return 0;        /* write offset 0, MOT */
    int got = 0;
    for (int i = 0; i < blocks * 128; i += 16) {
        if (i2c_aux(port, 0x5, NULL, 0, edid + i, 16)) break;   /* read, MOT */
        got = i + 16;
    }
    i2c_aux(port, 0x1, NULL, 0, NULL, 0);                       /* stop */
    return got;
}

/* ---- modes ----------------------------------------------------------------------- */
static int dtd(const u8 *b, dmode_t *m, int *hpos, int *vpos) {
    int clk = (b[0] | b[1] << 8) * 10;
    if (!clk) return 0;
    int ha = b[2] | (b[4] & 0xf0) << 4, hb = b[3] | (b[4] & 0x0f) << 8;
    int va = b[5] | (b[7] & 0xf0) << 4, vb = b[6] | (b[7] & 0x0f) << 8;
    int hso = b[8] | (b[11] & 0xc0) << 2, hsw = b[9] | (b[11] & 0x30) << 4;
    int vso = (b[10] >> 4) | (b[11] & 0x0c) << 2, vsw = (b[10] & 0xf) | (b[11] & 3) << 4;
    if (b[17] & 0x80) return 0;                                 /* interlaced */
    *m = (dmode_t){ clk, ha, ha + hso, ha + hso + hsw, ha + hb, va, va + vso, va + vso + vsw, va + vb, 0, 0 };
    int digital_sep = (b[17] & 0x18) == 0x18;
    *hpos = digital_sep ? (b[17] >> 1) & 1 : 1;
    *vpos = digital_sep ? (b[17] >> 2) & 1 : 1;
    return 1;
}

static int fits(const dmode_t *m, int rate, int lanes) {
    return m->clock <= 300000 && m->hd <= 4096 && m->vd <= 4096 && (u64)m->clock * 24 <= (u64)rate * lanes * 8;
}

static const dmode_t mode_1080p = { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1 };
static const dmode_t mode_720p = { 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, 1, 1 };

/* the preferred mode if the link carries it, else the largest one listed that it does */
static void pick_mode(const u8 *edid, int len, int rate, int lanes) {
    dmode_t best = { 0 };
    int have = 0;
    for (int blk = 0; blk * 128 + 128 <= len && blk < 2; blk++) {
        const u8 *e = edid + blk * 128;
        int start = blk == 0 ? 54 : (e[0] == 2 ? e[2] : 0), end = blk == 0 ? 126 : 127;
        if (blk && (e[0] != 2 || start < 4)) continue;
        for (int o = start; o + 18 <= end; o += 18) {
            dmode_t m;
            int hp, vp;
            if (!dtd(e + o, &m, &hp, &vp)) continue;
            m.hpos = hp; m.vpos = vp;
            if (!fits(&m, rate, lanes)) continue;
            if (!have) { best = m; have = 1; if (blk == 0 && o == 54) goto done; }
            else if ((u64)m.hd * m.vd > (u64)best.hd * best.vd) best = m;
        }
    }
done:
    if (!have) best = fits(&mode_1080p, rate, lanes) ? mode_1080p : mode_720p;
    D.m = best;
}

static void monitor_name(const u8 *e) {
    strlcpy(D.monitor, "monitor", sizeof D.monitor);
    for (int o = 54; o + 18 <= 126; o += 18)
        if (!e[o] && !e[o + 1] && e[o + 3] == 0xfc) {
            int n = 0;
            for (; n < 13 && e[o + 5 + n] != '\n' && e[o + 5 + n]; n++) D.monitor[n] = (char)e[o + 5 + n];
            while (n && D.monitor[n - 1] == ' ') n--;
            D.monitor[n] = 0;
        }
}

/* ---- the PHY and the PLL (intel_dpio_phy.c, chv_prepare_pll) --------------------------- */
static void lane_soft_reset(int reset) {
    int phy = pipe_phy(), ch = port_ch();
    u32 v = dpio_rd(phy, PCS01_DW0(ch));
    if (reset) v &= ~((1u << 16) | (1u << 7)); else v |= (1u << 16) | (1u << 7);
    dpio_wr(phy, PCS01_DW0(ch), v);
    if (D.lanes > 2) {
        v = dpio_rd(phy, PCS23_DW0(ch));
        if (reset) v &= ~((1u << 16) | (1u << 7)); else v |= (1u << 16) | (1u << 7);
        dpio_wr(phy, PCS23_DW0(ch), v);
    }
    v = dpio_rd(phy, PCS01_DW1(ch)) | (1u << 23);
    if (reset) v &= ~(1u << 5); else v |= 1u << 5;
    dpio_wr(phy, PCS01_DW1(ch), v);
    if (D.lanes > 2) {
        v = dpio_rd(phy, PCS23_DW1(ch)) | (1u << 23);
        if (reset) v &= ~(1u << 5); else v |= 1u << 5;
        dpio_wr(phy, PCS23_DW1(ch), v);
    }
}

static u32 unused_lanes(void) { return ~((1u << D.lanes) - 1) & 0xf; }

static void phy_pre_pll_enable(void) {
    int phy = pipe_phy(), ch = port_ch();
    if (ch == 0 && D.pipe == 1) D.cl2_release = !powergate_ch(0, 1, 1);   /* trick CL2 into life */
    powergate_lanes(port_phy(), ch, 1, unused_lanes());
    lane_soft_reset(1);
    u32 v;
    if (D.pipe != 1) {
        v = dpio_rd(phy, CMN_DW5_CH0) & ~((3u << 22) | (3u << 20));
        v |= ch == 0 ? 3u << 22 : 3u << 20;                             /* BUFLEFT / BUFRIGHT ENA1 force */
        dpio_wr(phy, CMN_DW5_CH0, v);
    } else {
        v = dpio_rd(phy, CMN_DW1_CH1) & ~((3u << 17) | (3u << 19));
        v |= ch == 0 ? 3u << 17 : 3u << 19;                             /* ENA2 force */
        dpio_wr(phy, CMN_DW1_CH1, v);
    }
    v = dpio_rd(phy, PCS01_DW8(ch)) | (1u << 20);                       /* used clock channel override */
    if (D.pipe != 1) v &= ~(1u << 21); else v |= 1u << 21;
    dpio_wr(phy, PCS01_DW8(ch), v);
    if (D.lanes > 2) {
        v = dpio_rd(phy, PCS23_DW8(ch)) | (1u << 20);
        if (D.pipe != 1) v &= ~(1u << 21); else v |= 1u << 21;
        dpio_wr(phy, PCS23_DW8(ch), v);
    }
    v = dpio_rd(phy, CMN_DW19(ch));
    if (D.pipe != 1) v &= ~(1u << 13); else v |= 1u << 13;
    dpio_wr(phy, CMN_DW19(ch), v);
}

static u32 dpll_val(void) {
    return DPLL_SSC_REF_CLK_CHV | DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS | DPLL_INTEGRATED_CRI_CLK | DPLL_VCO_ENABLE;
}
static u32 dpll_reg(void) { return D.pipe == 1 ? DPLL_B : DPLL_C; }

static int pll_enable(void) {
    int phy = pipe_phy(), pc = pipe_ch();
    /* the fixed DP dividers (intel_dp.c chv_dpll[]) */
    u32 p1 = 4, p2 = D.rate == 162000 ? 2 : 1, m2 = D.rate == 162000 ? 0x819999a : 0x6c00000;
    int vco = D.rate == 162000 ? 6480000 : 5400000;
    u32 m2_int = m2 >> 22, m2_frac = m2 & 0x3fffff;
    wr(dpll_reg(), dpll_val() & ~DPLL_VCO_ENABLE);                     /* ref clock + SSC */
    dpio_wr(phy, CMN_DW13(pc), 5u << 21 | p1 << 13 | p2 << 8 | 1u << 4);
    dpio_wr(phy, PLL_DW0(pc), m2_int);
    dpio_wr(phy, PLL_DW1(pc), 0u | 1u << 8);                           /* m1 = 2, n = 1 */
    dpio_wr(phy, PLL_DW2(pc), m2_frac);
    u32 v = dpio_rd(phy, PLL_DW3(pc)) & ~(0xfu | (1u << 16));
    v |= 2u;
    if (m2_frac) v |= 1u << 16;
    dpio_wr(phy, PLL_DW3(pc), v);
    v = dpio_rd(phy, PLL_DW9(pc)) & ~((7u << 1) | 1u);
    v |= 5u << 1;
    if (!m2_frac) v |= 1u;
    dpio_wr(phy, PLL_DW9(pc), v);
    u32 lf, tri;
    if (vco == 5400000) { lf = 3u | 8u << 8 | 1u << 16; tri = 9; }
    else { lf = 4u | 9u << 8 | 3u << 16; tri = 8; }                    /* 6480000 */
    dpio_wr(phy, PLL_DW6(pc), lf);
    v = dpio_rd(phy, PLL_DW8(pc)) & ~0x3ffu;
    dpio_wr(phy, PLL_DW8(pc), v | tri);
    dpio_wr(phy, CMN_DW14(pc), dpio_rd(phy, CMN_DW14(pc)) | (1u << 14));   /* AFC recal */
    /* _chv_enable_pll */
    dpio_wr(phy, CMN_DW14(pc), dpio_rd(phy, CMN_DW14(pc)) | (1u << 13));   /* 10-bit clock to the display */
    hal_delay_us(1);
    wr(dpll_reg(), dpll_val());
    if (!wait_reg(dpll_reg(), DPLL_LOCK_VLV, DPLL_LOCK_VLV, 2)) return 0;
    wr(CBR4_VLV, D.pipe == 1 ? 1u << 18 : 1u << 29);                   /* WaPixelRepeatModeFixForC0 */
    wr(DPLL_MD_B, 0);
    wr(CBR4_VLV, 0);
    return 1;
}

static void phy_pre_encoder_enable(void) {
    int phy = pipe_phy(), ch = port_ch();
    dpio_wr(phy, PCS01_DW11(ch), dpio_rd(phy, PCS01_DW11(ch)) & ~(1u << 3));
    if (D.lanes > 2) dpio_wr(phy, PCS23_DW11(ch), dpio_rd(phy, PCS23_DW11(ch)) & ~(1u << 3));
    for (int i = 0; i < D.lanes; i++) dpio_wr(phy, TX_DW14(ch, i), (u32)(D.lanes == 1 ? 0 : i == 1 ? 0 : 1) << 30);
    int stagger = D.rate > 135000 ? 0xd : 0x7;
    dpio_wr(phy, PCS01_DW11(ch), dpio_rd(phy, PCS01_DW11(ch)) | (0x1fu << 24));
    if (D.lanes > 2) dpio_wr(phy, PCS23_DW11(ch), dpio_rd(phy, PCS23_DW11(ch)) | (0x1fu << 24));
    dpio_wr(phy, PCS01_DW12(ch), (u32)stagger | 1u << 6 | 0x1fu << 8 | 6u << 16 | 0u << 20);
    if (D.lanes > 2) dpio_wr(phy, PCS23_DW12(ch), (u32)stagger | 1u << 6 | 0x1fu << 8 | 7u << 16 | 5u << 20);
    lane_soft_reset(0);
}

/* chv_signal_levels + chv_set_phy_signal_level */
static void signal_levels(void) {
    static const u8 deemph[4] = { 128, 85, 64, 43 };
    static const u8 margin[4][4] = { { 52, 77, 102, 154 }, { 78, 116, 154, 0 }, { 104, 154, 0, 0 }, { 154, 0, 0, 0 } };
    int vs = D.train[0] & 3, pe = (D.train[0] >> 3) & 3;
    u32 de = deemph[pe], mg = margin[pe][vs];
    if (!mg) mg = 154;
    int uniq = pe == 0 && vs == 3;
    int phy = pipe_phy(), ch = port_ch();
    u32 v = dpio_rd(phy, PCS01_DW10(ch)) & ~((3u << 30) | (0xfu << 24) | (0xfu << 16));
    dpio_wr(phy, PCS01_DW10(ch), v);
    if (D.lanes > 2) dpio_wr(phy, PCS23_DW10(ch), dpio_rd(phy, PCS23_DW10(ch)) & ~((3u << 30) | (0xfu << 24) | (0xfu << 16)));
    dpio_wr(phy, PCS01_DW9(ch), dpio_rd(phy, PCS01_DW9(ch)) & ~((7u << 13) | (7u << 10)));
    if (D.lanes > 2) dpio_wr(phy, PCS23_DW9(ch), dpio_rd(phy, PCS23_DW9(ch)) & ~((7u << 13) | (7u << 10)));
    for (int i = 0; i < D.lanes; i++) {
        dpio_wr(phy, TX_DW4(ch, i), (dpio_rd(phy, TX_DW4(ch, i)) & ~(0xffu << 24)) | de << 24);
        u32 t = dpio_rd(phy, TX_DW2(ch, i)) & ~(0xffu << 16) & ~(0xffu << 8);
        dpio_wr(phy, TX_DW2(ch, i), t | mg << 16 | 0x9au << 8);
        t = dpio_rd(phy, TX_DW3(ch, i));
        if (uniq) t |= 1u << 27; else t &= ~(1u << 27);
        dpio_wr(phy, TX_DW3(ch, i), t);
    }
    dpio_wr(phy, PCS01_DW10(ch), dpio_rd(phy, PCS01_DW10(ch)) | (3u << 30));   /* start swing calculation */
    if (D.lanes > 2) dpio_wr(phy, PCS23_DW10(ch), dpio_rd(phy, PCS23_DW10(ch)) | (3u << 30));
}

/* ---- link training (intel_dp_link_training.c) ----------------------------------------- */
static u32 port_reg(void) { return DB + 0x64000 + 0x100u * (u32)D.port; }

static void set_pattern(int tp) {
    D.dp &= ~DP_LINK_TRAIN_MASK_CHV;
    D.dp |= tp == 1 ? DP_LINK_TRAIN_PAT_1 : tp == 2 ? DP_LINK_TRAIN_PAT_2 : DP_LINK_TRAIN_OFF;
    wr(port_reg(), D.dp);
    (void)rd(port_reg());
}

static int set_train(int tp) {
    set_pattern(tp);
    u8 b[5] = { (u8)(tp ? tp | 0x20 : 0) };                             /* scrambling off while training */
    if (!tp) return dpcd_write(D.port, 0x102, b, 1) == 1;
    memcpy(b + 1, D.train, (usize)D.lanes);
    return dpcd_write(D.port, 0x102, b, 1 + D.lanes) == 1 + D.lanes;
}

static int link_status(u8 *st) { return dpcd_read(D.port, 0x202, st, 6) == 6; }
static int lane_bits(const u8 *st, int lane) { return (st[lane >> 1] >> (4 * (lane & 1))) & 0xf; }

static void adjust(const u8 *st) {
    int v = 0, p = 0;
    for (int l = 0; l < D.lanes; l++) {
        int a = (st[4 + (l >> 1)] >> (4 * (l & 1))) & 0xf;
        if ((a & 3) > v) v = a & 3;
        if (((a >> 2) & 3) > p) p = (a >> 2) & 3;
    }
    u8 t = (u8)v;
    if (v >= 3) t = 3 | 4;                                              /* MAX_SWING_REACHED */
    int pmax = v == 0 ? 3 : v == 1 ? 2 : v == 2 ? 1 : 0;
    if (p >= pmax) t |= (u8)(pmax << 3 | 0x20); else t |= (u8)(p << 3);
    for (int l = 0; l < 4; l++) D.train[l] = t;
}

static int train_link(void) {
    u8 cfg[2] = { (u8)(D.rate == 162000 ? 0x06 : 0x0a), (u8)(D.lanes | ((D.dpcd[2] & 0x80) ? 0x80 : 0)) };
    dpcd_write(D.port, 0x100, cfg, 2);
    u8 ds[2] = { 0, 1 };                                                /* no downspread, 8b/10b */
    dpcd_write(D.port, 0x107, ds, 2);
    memset(D.train, 0, sizeof D.train);
    signal_levels();
    if (!set_train(1)) return 0;
    /* clock recovery */
    int same = 1, maxed = 0, cr = 0;
    u8 st[6];
    for (int it = 0; it < 32; it++) {
        hal_delay_us(100);
        if (!link_status(st)) return 0;
        cr = 1;
        for (int l = 0; l < D.lanes; l++) if (!(lane_bits(st, l) & 1)) cr = 0;
        if (cr) break;
        if (same == 5 || maxed) break;
        u8 old = D.train[0] & 3;
        adjust(st);
        signal_levels();
        if (dpcd_write(D.port, 0x103, D.train, D.lanes) != D.lanes) return 0;
        same = (D.train[0] & 3) == old ? same + 1 : 1;
        if (D.train[0] & 4) maxed++;
    }
    if (!cr) return 0;
    /* channel equalization with pattern 2 */
    if (!set_train(2)) return 0;
    int eq = 0;
    for (int tr = 0; tr < 5; tr++) {
        int iv = D.dpcd[0x0e] & 0x7f;
        hal_delay_us(iv ? (u32)iv * 4000 : 400);
        if (!link_status(st)) break;
        int ok = 1;
        for (int l = 0; l < D.lanes; l++) {
            int b = lane_bits(st, l);
            if (!(b & 1)) { ok = -1; break; }
            if ((b & 7) != 7) ok = 0;
        }
        if (ok < 0) break;
        if (ok && (st[2] & 1)) { eq = 1; break; }
        adjust(st);
        signal_levels();
        if (dpcd_write(D.port, 0x103, D.train, D.lanes) != D.lanes) break;
    }
    set_train(0);
    return eq;
}

/* ---- the pipe and plane ------------------------------------------------------------------ */
static u32 T(u32 off) { return DB + (D.pipe == 1 ? 0x61000u : 0x63000u) + off; }   /* transcoder */
static u32 P(u32 off) { return DB + (D.pipe == 1 ? 0x71000u : 0x74000u) + off; }   /* pipe, plane */

static void compute_m_n(u64 m, u64 n, u32 *rm, u32 *rn) {
    u64 nn = 1;
    while (nn < n) nn <<= 1;
    if (nn > 0x800000) nn = 0x800000;
    u64 mm = m * nn / n;
    while (mm > 0xffffff || nn > 0xffffff) { mm >>= 1; nn >>= 1; }
    *rm = (u32)mm; *rn = (u32)nn;
}

static void set_timings(void) {
    const dmode_t *m = &D.m;
    u32 dm, dn, lm, ln;
    compute_m_n((u64)24 * m->clock, (u64)D.rate * D.lanes * 8, &dm, &dn);
    compute_m_n((u64)m->clock, (u64)D.rate, &lm, &ln);
    wr(T(0x30), (63u << 25) | dm);                                       /* TU size 64 */
    wr(T(0x34), dn);
    wr(T(0x40), lm);
    wr(T(0x44), ln);
    wr(T(0x28), 0);                                                      /* VSYNCSHIFT */
    wr(T(0x00), (u32)(m->hd - 1) | (u32)(m->ht - 1) << 16);
    wr(T(0x04), (u32)(m->hd - 1) | (u32)(m->ht - 1) << 16);
    wr(T(0x08), (u32)(m->hss - 1) | (u32)(m->hse - 1) << 16);
    wr(T(0x0c), (u32)(m->vd - 1) | (u32)(m->vt - 1) << 16);
    wr(T(0x10), (u32)(m->vd - 1) | (u32)(m->vt - 1) << 16);
    wr(T(0x14), (u32)(m->vss - 1) | (u32)(m->vse - 1) << 16);
    wr(T(0x1c), (u32)(m->hd - 1) << 16 | (u32)(m->vd - 1));             /* PIPESRC */
    if (D.pipe == 1) { wr(DB + 0x61a00, 0); wr(DB + 0x61a04, 0); }       /* CHV_BLEND legacy, CHV_CANVAS */
    wr(P(0x08), 0);                                                      /* PIPECONF: 8 bpc, progressive, off */
}

/* this pipe's share of the display FIFO and its watermark (intel_pm.c) */
static void fifo_and_wm(void) {
    wr(FW_BLC_SELF_VLV, rd(FW_BLC_SELF_VLV) & ~(1u << 15));             /* no CxSR with two pipes */
    u32 wm = 511 - 64;
    if (D.pipe == 1) {
        u32 a = rd(DSPARB) & ~(0xffu << 16) & ~(0xffu << 24);
        wr(DSPARB, a | (511u & 0xff) << 16 | (511u & 0xff) << 24);       /* sprites C/D get nothing */
        u32 a2 = rd(DSPARB2) & ~(0xfu << 8) & ~(0xfu << 12);
        wr(DSPARB2, a2 | 1u << 8 | 1u << 12);
        wr(DSPFW1, (rd(DSPFW1) & ~(0xffu << 8)) | (wm & 0xff) << 8);
        wr(DSPHOWM, (rd(DSPHOWM) & ~(1u << 12)) | (wm >> 8) << 12);
    } else {
        u32 a = rd(DSPARB3) & ~0xffffu;
        wr(DSPARB3, a | (511u & 0xff) | (511u & 0xff) << 8);
        u32 a2 = rd(DSPARB2) & ~(0xfu << 16) & ~(0xfu << 20);
        wr(DSPARB2, a2 | 1u << 16 | 1u << 20);
        wr(DSPFW9_CHV, (rd(DSPFW9_CHV) & ~(0xffu << 16)) | (wm & 0xff) << 16);
        wr(DSPHOWM, (rd(DSPHOWM) & ~(1u << 21)) | (wm >> 8) << 21);
    }
    wr(DB + 0x70050 + 4u * (u32)D.pipe, 0);                              /* VLV_DDL: no drain latency */
}

static int alloc_fb(void) {
    D.pitch = (u32)((D.m.hd * 4 + 63) & ~63);
    usize bytes = (usize)D.pitch * (usize)D.m.vd;
    if (D.fb && D.fb_bytes >= bytes) { memset(D.fb, 0, bytes); }
    else {
        D.fb = hal_dma_alloc(bytes);                                     /* kept for later re-plugs */
        if (!D.fb) return 0;
        D.fb_bytes = bytes;
    }
    u32 gtt = gpu_scanout_gtt(D.fb_bytes);
    if (!gtt) return 0;
    gpu_gtt_map(gtt, (u64)(usize)D.fb, (D.fb_bytes + 4095) / 4096);
    D.fb_gtt = gtt;
    for (usize a = (usize)D.fb & ~(usize)63; a < (usize)D.fb + bytes; a += 64) __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
    D.last_w = D.last_h = 0;
    return 1;
}

static void plane_enable(void) {
    if (D.pipe == 1) {
        wr(DB + 0x61a0c, (u32)(D.m.vd - 1) << 16 | (u32)(D.m.hd - 1));   /* PRIMSIZE */
        wr(DB + 0x61a08, 0);                                             /* PRIMPOS */
        wr(DB + 0x61a10, 0);                                             /* PRIMCNSTALPHA */
    }
    wr(P(0x180), (1u << 31) | (0x6u << 26));                             /* enable, BGRX 8:8:8:8, no gamma */
    wr(P(0x188), D.pitch);
    wr(P(0x1a4), 0);                                                     /* TILEOFF */
    wr(P(0x184), 0);                                                     /* LINOFF */
    wr(P(0x19c), D.fb_gtt);                                              /* SURF: armed at the next vblank */
}

/* ---- enable / disable --------------------------------------------------------------------- */
static int enable(void) {
    D.pipe = D.port == 3 ? 2 : 1;
    if (!cmn_enable(port_phy())) { say("External display: %s", "the display PHY did not power up"); return 0; }
    set_timings();
    fifo_and_wm();
    /* intel_dp_prepare */
    D.dp = (rd(port_reg()) & DP_DETECTED) | (u32)(D.lanes - 1) << 19 | DP_LINK_TRAIN_OFF | (u32)D.pipe << 16;
    if (D.dpcd[2] & 0x80) D.dp |= DP_ENHANCED_FRAMING;
    if (D.m.hpos) D.dp |= DP_SYNC_HS_HIGH;
    if (D.m.vpos) D.dp |= DP_SYNC_VS_HIGH;
    phy_pre_pll_enable();
    if (!pll_enable()) { say("External display: %s", "the PLL did not lock"); return 0; }
    phy_pre_encoder_enable();
    /* intel_enable_dp: the port with pattern 1, then ready lanes, sink on, train */
    set_pattern(1);
    D.dp |= DP_PORT_EN;
    wr(port_reg(), D.dp);
    (void)rd(port_reg());
    u32 rmask = unused_lanes(), rreg = D.port == 3 ? DPIO_PHY_STATUS : DPLL_A, rsh = D.port == 2 ? 4 : 0;
    if (!wait_reg(rreg, 0xfu << rsh, rmask << rsh, 1000)) klog("display: port lanes not ready (%08x)", rd(rreg));
    dpcd_wb(D.port, 0x600, 1);                                           /* sink power D0 */
    hal_delay_us(1000);
    if (!train_link()) { say("External display: %s", "DisplayPort link training failed"); return 0; }
    if (D.cl2_release) { powergate_ch(0, 1, 0); D.cl2_release = 0; }
    if (!alloc_fb()) { say("External display: %s", "no memory for its picture"); return 0; }
    wr(P(0x08), 1u << 31);                                               /* PIPECONF enable */
    if (!wait_reg(P(0x08), 1u << 30, 1u << 30, 100)) { say("External display: %s", "the pipe did not start"); return 0; }
    plane_enable();
    return 1;
}

static void disable(void) {
    if (!D.pipe) return;
    wr(P(0x180), 0);                                                     /* plane off */
    wr(P(0x19c), 0);
    wr(P(0x08), rd(P(0x08)) & ~(1u << 31));                              /* pipe off */
    wait_reg(P(0x08), 1u << 30, 0, 100);
    D.dp = (D.dp & ~DP_LINK_TRAIN_MASK_CHV) | DP_LINK_TRAIN_OFF;          /* link idle */
    wr(port_reg(), D.dp);
    D.dp &= ~DP_PORT_EN;
    wr(port_reg(), D.dp);
    lane_soft_reset(1);
    wr(dpll_reg(), DPLL_SSC_REF_CLK_CHV | DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS | DPLL_INTEGRATED_CRI_CLK);
    int phy = pipe_phy(), pc = pipe_ch();
    dpio_wr(phy, CMN_DW14(pc), dpio_rd(phy, CMN_DW14(pc)) & ~(1u << 13));
    powergate_lanes(port_phy(), port_ch(), 0, 0);
    D.pipe = 0;
}

/* ---- detection ------------------------------------------------------------------------- */
static int probe_port(int port) {
    if (!cmn_enable(port == 3)) return 0;
    u8 c[16];
    if (dpcd_read(port, 0, c, 16) != 16 || !c[0] || c[0] == 0xff) return 0;
    memcpy(D.dpcd, c, 16);
    return 1;
}

static int sink_present(void) {
    u8 c;
    return dpcd_read(D.port, 0, &c, 1) == 1 && c;
}

static void connect(int port) {
    D.port = port;
    int maxrate = D.dpcd[1] >= 0x0a ? 270000 : 162000, maxl = D.dpcd[2] & 0x1f;
    if (maxl != 1 && maxl != 2 && maxl != 4) maxl = maxl >= 4 ? 4 : maxl >= 2 ? 2 : 1;
    u8 edid[256];
    memset(edid, 0, sizeof edid);
    int len = read_edid(port, edid, 2);
    if (len < 128 || memcmp(edid, "\x00\xff\xff\xff\xff\xff\xff\x00", 8)) { len = 0; strlcpy(D.monitor, "monitor (no EDID)", sizeof D.monitor); }
    else monitor_name(edid);
    /* the lowest link rate and lane count that carry the mode (Linux 4.9 order) */
    pick_mode(edid, len, maxrate, maxl);
    D.rate = 0;
    for (int r = 162000; r <= maxrate && !D.rate; r += 108000)
        for (int l = 1; l <= maxl && !D.rate; l <<= 1)
            if (fits(&D.m, r, l)) { D.rate = r; D.lanes = l; }
    if (!D.rate) { D.rate = maxrate; D.lanes = maxl; }
    klog("display: DP %c: %s, DPCD %x.%x, up to %u.%u Gbps x%d; mode %dx%d @ %d kHz, link %d.%02d Gbps x%d",
         'A' + port, D.monitor, D.dpcd[0] >> 4, D.dpcd[0] & 15, maxrate / 100000, maxrate / 10000 % 10, maxl,
         D.m.hd, D.m.vd, D.m.clock, D.rate / 100000, D.rate / 1000 % 100, D.lanes);
    D.busy = 1;
    hal_setting_set(u"QrtDispGuard", 1);
    int ok = enable();
    hal_setting_set(u"QrtDispGuard", 0);
    if (!ok) { disable(); D.busy = 0; D.state = D_FAILED; return; }
    u64 tot = (u64)D.m.ht * D.m.vt;
    u32 hz = (u32)(((u64)D.m.clock * 1000 + tot / 2) / tot);
    fmt(D.status, sizeof D.status, "%s: %dx%d at %u Hz (DisplayPort %c, %d lane%s)", D.monitor, D.m.hd, D.m.vd, hz,
        'A' + port, D.lanes, D.lanes == 1 ? "" : "s");
    klog("display: %s", D.status);
    D.state = D_ON;
    D.busy = 0;
    shell_redraw();
}

static void display_thread(void *arg) {
    (void)arg;
    /* hrawclk for the AUX bit clock (vlv_hrawclk), 2 MHz wanted */
    u32 hpll = (u32[]){ 800, 1600, 2000, 2400 }[cck_rd(0x8) & 3] * 1000;
    u32 v = cck_rd(0x6c), div = v & 0x1f;
    u32 raw = (hpll * 2 + (div + 1) / 2) / (div + 1);
    if (raw < 100000 || raw > 400000) raw = 200000;
    aux_div = (raw + 1000) / 2000;
    for (u32 p = DPLL_B; p <= DPLL_C; p += DPLL_C - DPLL_B) {             /* CRI + ref clocks for pipes B, C */
        if (rd(p) & DPLL_VCO_ENABLE) continue;
        wr(p, rd(p) | DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS | DPLL_INTEGRATED_CRI_CLK);
    }
    phy_ctl_init();
    klog("display: watching DisplayPort B, C, D (hrawclk %u kHz, AUX divider %u)", raw, aux_div);
    strlcpy(D.status, "No external display connected", sizeof D.status);
    D.state = D_SEARCHING;
    int misses = 0, tick = 0;
    for (;;) {
        msleep(1000);
        if (D.state == D_DISABLED) {
            if (D.pipe) { D.busy = 1; msleep(50); disable(); D.busy = 0; }
            continue;
        }
        if (D.state == D_ON || D.state == D_FAILED) {
            if (sink_present()) { misses = 0; continue; }
            if (++misses < 2) continue;                                  /* unplugged */
            D.busy = 1;
            msleep(50);
            disable();
            D.busy = 0;
            D.state = D_SEARCHING;
            strlcpy(D.status, "No external display connected", sizeof D.status);
            klog("display: the monitor was unplugged");
            shell_redraw();
            continue;
        }
        /* ports whose hot-plug pin is high first; every 5 s all of them over AUX, in case the
         * dock's HPD is not wired to the live-status bits */
        u32 live = rd(PORT_HOTPLUG_STAT);
        static const u32 bit[4] = { 0, 1u << 29, 1u << 28, 1u << 27 };
        int all = ++tick % 5 == 0;
        for (int port = 1; port <= 3; port++)
            if ((all || (live & bit[port])) && probe_port(port)) { connect(port); break; }
    }
}

void display_start(void) {
    if (k.native && !k.is_venue && ivb_present_supported()) { ivb_display_start(); return; }   /* Ivy Bridge HDMI: ivb.c */
    if (!k.native || !k.is_venue || D.R) return;
    D.R = gpu_regs();
    if (!D.R) { strlcpy(D.status, "Off: the graphics registers are not mapped", sizeof D.status); return; }
    if (hal_setting_get(u"QrtDispGuard", 0)) {
        hal_setting_set(u"QrtDispGuard", 0);
        strlcpy(D.status, "Off for this boot: setting up the external display froze the tablet last time", sizeof D.status);
        D.state = D_FAILED;
        return;
    }
    if (hal_setting_get(u"QrtExtDisplay", 1) == 0) { D.state = D_DISABLED; strlcpy(D.status, "Off", sizeof D.status); }
    thread_create("display", display_thread, NULL, 0);
}

const char *display_status(void) {
    if (D.virt) return D.status;
    if (!k.native) return "Needs the native kernel";
    if (!k.is_venue && ivb_present_supported()) return ivb_display_status();
    if (!k.is_venue) return "Only on the Venue 8 Pro 5855 (Cherry Trail DisplayPort) and Ivy Bridge PCs (HDMI)";
    if (!D.R && !D.status[0]) return "Not started";
    return D.status;
}

int display_enabled(void) { return D.state != D_DISABLED; }

void display_set_enabled(int on) {
    hal_setting_set(u"QrtExtDisplay", on ? 1 : 0);
    if (!D.R) return;
    if (!on) { D.state = D_DISABLED; strlcpy(D.status, "Off", sizeof D.status); }
    else if (D.state == D_DISABLED) { D.state = D_SEARCHING; strlcpy(D.status, "No external display connected", sizeof D.status); }
}

int display_connected(void) { return D.state == D_ON && D.fb && !D.busy; }
int display_size(int *w, int *h) { *w = D.m.hd; *h = D.m.vd; return display_connected(); }
const char *display_monitor(void) { return D.monitor; }

/* tests (QEMU has no DisplayPort): a monitor that is only a buffer in memory */
void display_virtual(int w, int h) {
    if (k.is_venue) return;
    if (w <= 0 || h <= 0) { D.state = D_SEARCHING; strlcpy(D.status, "No external display connected", sizeof D.status); klog("display: the virtual monitor was unplugged"); return; }
    w = MIN(w, 3840); h = MIN(h, 2160);
    usize bytes = (usize)w * 4 * (usize)h;
    if (!D.fb || D.fb_bytes < bytes) { D.fb = hal_dma_alloc(bytes); D.fb_bytes = D.fb ? bytes : 0; }
    if (!D.fb) return;
    D.virt = 1;
    D.fb_gtt = 0;                                                        /* not scanned out: CPU copies only */
    D.m = (dmode_t){ 148500, w, w, w, w, h, h, h, h, 1, 1 };
    D.pitch = (u32)w * 4;
    D.last_w = D.last_h = 0;
    strlcpy(D.monitor, "Test monitor", sizeof D.monitor);
    fmt(D.status, sizeof D.status, "%s: %dx%d (test)", D.monitor, w, h);
    klog("display: %s", D.status);
    D.state = D_ON;
}

/* a checksum of the monitor's picture, for tests */
u32 display_checksum(void) {
    if (D.state != D_ON || !D.fb) return 0;
    u32 sum = 0;
    for (int y = 0; y < D.m.vd; y += 3) {
        const u32 *row = (const u32 *)((const u8 *)D.fb + (usize)y * D.pitch);
        for (int x = 0; x < D.m.hd; x += 7) sum = sum * 31 + (row[x] & 0xffffff);
    }
    return sum;
}

/* ---- mirroring --------------------------------------------------------------------------- */
void display_mirror(const u32 *px, int w, int h, int stride, int x, int y, int rw, int rh) {
    if (D.state != D_ON || D.busy || !D.fb) return;
    int W = D.m.hd, H = D.m.vd;
    if (w != D.last_w || h != D.last_h) {                                /* new canvas size: refit, redraw all */
        D.last_w = w; D.last_h = h;
        D.scale = MIN((float)W / w, (float)H / h);
        D.x0 = (W - (int)(w * D.scale)) / 2;
        D.y0 = (H - (int)(h * D.scale)) / 2;
        memset(D.fb, 0, (usize)D.pitch * H);
        for (usize a = (usize)D.fb & ~(usize)63; a < (usize)D.fb + (usize)D.pitch * H; a += 64) __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
        x = 0; y = 0; rw = w; rh = h;
    }
    /* the destination rectangle, one pixel wider for the bilinear filter */
    int dx0 = D.x0 + (int)(x * D.scale) - 1, dy0 = D.y0 + (int)(y * D.scale) - 1;
    int dx1 = D.x0 + (int)((x + rw) * D.scale + 0.999f) + 1, dy1 = D.y0 + (int)((y + rh) * D.scale + 0.999f) + 1;
    int lx = D.x0, ly = D.y0, hx = D.x0 + (int)(w * D.scale), hy = D.y0 + (int)(h * D.scale);
    if (dx0 < lx) dx0 = lx;
    if (dy0 < ly) dy0 = ly;
    if (dx1 > hx) dx1 = hx;
    if (dy1 > hy) dy1 = hy;
    if (dx1 <= dx0 || dy1 <= dy0) return;
    if (D.fb_gtt && gpu_present_scaled(px, w, h, stride, D.fb_gtt, W, H, (int)D.pitch, D.x0, D.y0, D.scale, dx0, dy0, dx1 - dx0, dy1 - dy0)) {
        D.frames++;
        return;
    }
    /* CPU: nearest neighbour */
    float inv = 1.0f / D.scale;
    u32 *row0 = D.fb;
    for (int Y = dy0; Y < dy1; Y++) {
        int sy = (int)((Y - D.y0) * inv);
        if (sy >= h) sy = h - 1;
        const u32 *s = px + (usize)sy * stride;
        u32 *d = (u32 *)((u8 *)row0 + (usize)Y * D.pitch);
        for (int X = dx0; X < dx1; X++) {
            int sx = (int)((X - D.x0) * inv);
            d[X] = s[sx < w ? sx : w - 1];
        }
        for (usize a = (usize)(d + dx0) & ~(usize)63; a < (usize)(d + dx1); a += 64) __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
    }
    D.frames++;
}

#else
void display_start(void) {}
const char *display_status(void) { return "Needs the 64-bit native kernel"; }
int display_enabled(void) { return 0; }
void display_set_enabled(int on) { (void)on; }
void display_mirror(const u32 *px, int w, int h, int stride, int x, int y, int rw, int rh) {}
int display_connected(void) { return 0; }
int display_size(int *w, int *h) { *w = *h = 0; return 0; }
const char *display_monitor(void) { return ""; }
void display_virtual(int w, int h) { (void)w; (void)h; }
u32 display_checksum(void) { return 0; }
#endif
