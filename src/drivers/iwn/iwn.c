/*
 * iwn.c - Intel Centrino Advanced-N 6230/6235 (the Panasonic FZ-G1's Wi-Fi) for QRT.
 *
 * A port of the parts of OpenBSD's iwn(4) that a client station needs, for the
 * 6000-series "g2b" adapters (firmware iwlwifi-6000g2b, which OpenBSD calls iwn-6030):
 * bring-up and the EEPROM (MAC address, channels, crystal calibration), the
 * initialization firmware's calibration and the runtime firmware it hands its results
 * to, Bluetooth coexistence, RXON, nodes and link quality, scanning, association,
 * the TX/RX rings and hardware CCMP for the pairwise key.  Register definitions,
 * firmware structures and tables come unchanged from OpenBSD's if_iwnreg.h; the
 * driver code follows if_iwn.c function by function (named in the comments).
 *
 * Differences from iwn(4), as in QRT's iwm port:
 *   - no interrupts: the driver polls IWN_INT and the RX status area (INTx is off in
 *     PCI config, so latched causes never reach the CPU), and no ICT table;
 *   - legacy (non-HT) rates only (the firmware's link-quality table retries down the
 *     AP's rates), no aggregation, no sensitivity tuning after the first setting;
 *   - net80211's work (scan results, MLME, WPA) is in src/net/wlan.c, through the same
 *     calls as the iwm driver (src/drivers/wifi.c picks the card).
 *
 * Original copyright:
 *
 * Copyright (c) 2007-2010 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "iwn_compat.h"
#include "iwn.h"
#include "../../kernel/vfs.h"
#include "../../net/wifilog.h"

#define LOG(...) wifilog("iwn: " __VA_ARGS__)

#define FW_PATH     "/lib/firmware/iwlwifi-6000g2b-6.ucode"
#define SLOT        4096                     /* per-entry command / frame buffer */
#define NTXQ        IWN5000_NTXQUEUES
#define CMD_QUEUE   4
#define DATA_QUEUE  0                        /* EDCA best effort (FIFO 3): all our frames */
#define SCRATCH_OFF 12                       /* where iwn_cmd_data.scratch sits in a slot */

enum { F_OTPROM = 1, F_5GHZ = 2, F_ADV_BT_COEX = 4, F_ENH_SENS = 8, F_CALIB_DONE = 16 };

typedef struct { u8 *va; u64 pa; usize size; } dma_t;
typedef struct {
    dma_t desc, slots;
    int qid, cur, queued;
    u8 used[IWN_TX_RING_COUNT];
} txring_t;

static struct {
    int present, attached, ready, fatal;
    pci_dev_t *pci;
    volatile u8 *regs;
    int hw_type, cap_off;
    u32 flags;
    char status[160], fwver[40];

    /* firmware file: the initialization and runtime images */
    const u8 *fw; usize fw_len;
    struct { const u8 *text, *data; u32 textsz, datasz; } main, init;
    u32 tlv_feature_flags, reset_noise_gain, noise_gain;

    /* EEPROM */
    u8 macaddr[6];
    u16 rfcfg; u8 txchainmask, rxchainmask, ntxchains, nrxchains;
    u32 prom_base, eeprom_crystal;
    u8 calib_ver;
    char domain[5];
    u8 chans[64], passive[64]; int nchans;

    /* DMA */
    dma_t fw_dma, kw, sched;
    txring_t txq[NTXQ];
    struct { dma_t desc, stat, bufs; int cur; } rxq;

    /* calibration results of the initialization firmware, for the runtime one */
    struct { u8 *buf; int len; } calib[5];

    /* runtime */
    int nic_locks;
    volatile int fw_dma_done, alive, calib_done;
    volatile u8 cmd_done[IWN_TX_RING_COUNT];
    u32 errptr;
    struct iwn_rxon rxon;
    struct iwn_rx_stat last_rx_stat; int last_rx_valid;

    /* association */
    int scanning, scan_5ghz, associated;
    u8 bssid[6];
    int channel;
    u64 tsc;
    int have_ptk;
    u8 ptk_tk[16];
    u8 rates[16]; int nrates;                    /* the AP's rates, for link quality */
    iwm_rx_fn rx;
    u64 rx_frames, tx_frames, tx_fail, rx_dropped;
    u32 missed_beacons;
} sc;

/* Received frames wait here until iwn_poll() hands them over (see iwm.c: never from
 * inside service(), which also runs while a command waits). */
#define RXQ_N     64
#define RXQ_FRAME 2560
static struct { u16 len; iwm_rxinfo_t ri; u8 frame[RXQ_FRAME]; } rxq_buf[RXQ_N];
static int rxq_head, rxq_count;

/* ---- register access (iwn_nic_lock, iwn_prph_*, iwn_mem_*) ---------------------------- */
static inline u32 RD(u32 reg) { return *(volatile u32 *)(sc.regs + reg); }
static inline void WR(u32 reg, u32 v) { *(volatile u32 *)(sc.regs + reg) = v; }
static inline void SETB(u32 reg, u32 m) { WR(reg, RD(reg) | m); }
static inline void CLRB(u32 reg, u32 m) { WR(reg, RD(reg) & ~m); }
static inline void barrier(void) { __asm__ volatile("mfence" ::: "memory"); }
static void DELAY(u32 us) { hal_delay_us(us); }

static dma_t dma_new(usize size) {
    dma_t d = { 0 };
    d.va = hal_dma_alloc(size);
    d.pa = (u64)(usize)d.va;
    d.size = size;
    return d;
}

static int nic_lock(void) {
    if (sc.nic_locks++ > 0) return 0;
    SETB(IWN_GP_CNTRL, IWN_GP_CNTRL_MAC_ACCESS_REQ);
    for (int i = 0; i < 1000; i++) {
        if ((RD(IWN_GP_CNTRL) & (IWN_GP_CNTRL_MAC_ACCESS_ENA | IWN_GP_CNTRL_SLEEP)) == IWN_GP_CNTRL_MAC_ACCESS_ENA) return 0;
        DELAY(10);
    }
    sc.nic_locks--;
    LOG("could not lock the NIC");
    return -1;
}
static void nic_unlock(void) {
    if (sc.nic_locks <= 0) return;
    if (--sc.nic_locks == 0) CLRB(IWN_GP_CNTRL, IWN_GP_CNTRL_MAC_ACCESS_REQ);
}
static u32 prph_read(u32 addr) { WR(IWN_PRPH_RADDR, IWN_PRPH_DWORD | addr); barrier(); return RD(IWN_PRPH_RDATA); }
static void prph_write(u32 addr, u32 v) { WR(IWN_PRPH_WADDR, IWN_PRPH_DWORD | addr); barrier(); WR(IWN_PRPH_WDATA, v); }
static void prph_setbits(u32 addr, u32 m) { prph_write(addr, prph_read(addr) | m); }
static void prph_clrbits(u32 addr, u32 m) { prph_write(addr, prph_read(addr) & ~m); }
static u32 mem_read(u32 addr) { WR(IWN_MEM_RADDR, addr); barrier(); return RD(IWN_MEM_RDATA); }
static void mem_write(u32 addr, u32 v) { WR(IWN_MEM_WADDR, addr); barrier(); WR(IWN_MEM_WDATA, v); }

static u32 pcie_lcsr(void) {
    pci_dev_t *p = sc.pci;
    return sc.cap_off ? pci_read32(p->bus, p->dev, p->fn, (u16)(sc.cap_off + 0x10)) : 0;
}

/* ---- the ROM (iwn_eeprom_lock, iwn_init_otprom, iwn_read_prom_data) --------------------- */
static int eeprom_lock(void) {
    for (int i = 0; i < 100; i++) {
        SETB(IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_EEPROM_LOCKED);
        for (int n = 0; n < 100; n++) {
            if (RD(IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_EEPROM_LOCKED) return 0;
            DELAY(10);
        }
    }
    return -1;
}
static void eeprom_unlock(void) { CLRB(IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_EEPROM_LOCKED); }

static int clock_wait(void) {
    SETB(IWN_GP_CNTRL, IWN_GP_CNTRL_INIT_DONE);
    for (int i = 0; i < 2500; i++) {
        if (RD(IWN_GP_CNTRL) & IWN_GP_CNTRL_MAC_CLOCK_READY) return 0;
        DELAY(10);
    }
    LOG("timeout waiting for clock stabilization");
    return -1;
}

static int init_otprom(void) {
    if (clock_wait() || nic_lock()) return -1;
    prph_setbits(IWN_APMG_PS, IWN_APMG_PS_RESET_REQ);
    DELAY(5);
    prph_clrbits(IWN_APMG_PS, IWN_APMG_PS_RESET_REQ);
    nic_unlock();
    SETB(IWN_DBG_LINK_PWR_MGMT, IWN_RESET_LINK_PWR_MGMT_DIS);   /* OTP shadow RAM (not a 1000) */
    CLRB(IWN_EEPROM_GP, IWN_EEPROM_GP_IF_OWNER);
    SETB(IWN_OTP_GP, IWN_OTP_GP_ECC_CORR_STTS | IWN_OTP_GP_ECC_UNCORR_STTS);
    return 0;
}

static int read_prom(u32 addr, void *data, int count) {
    u8 *out = data;
    addr += sc.prom_base;
    for (; count > 0; count -= 2, addr++) {
        WR(IWN_EEPROM, addr << 2);
        u32 val = 0;
        int n;
        for (n = 0; n < 10; n++) {
            val = RD(IWN_EEPROM);
            if (val & IWN_EEPROM_READ_VALID) break;
            DELAY(5);
        }
        if (n == 10) { LOG("timeout reading ROM at 0x%x", addr); return -1; }
        if (sc.flags & F_OTPROM) {
            u32 t = RD(IWN_OTP_GP);
            if (t & IWN_OTP_GP_ECC_UNCORR_STTS) { LOG("OTPROM ECC error at 0x%x", addr); return -1; }
            if (t & IWN_OTP_GP_ECC_CORR_STTS) SETB(IWN_OTP_GP, IWN_OTP_GP_ECC_CORR_STTS);
        }
        *out++ = (u8)(val >> 16);
        if (count > 1) *out++ = (u8)(val >> 24);
    }
    return 0;
}

/* ---- power (iwn_apm_init, iwn_apm_stop, iwn5000_nic_config, iwn_hw_prepare) ------------ */
static int apm_init(void) {
    SETB(IWN_GIO_CHICKEN, IWN_GIO_CHICKEN_DIS_L0S_TIMER);
    SETB(IWN_GIO_CHICKEN, IWN_GIO_CHICKEN_L1A_NO_L0S_RX);
    SETB(IWN_DBG_HPET_MEM, 0xffff0000);
    SETB(IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_HAP_WAKE_L1A);
    if (pcie_lcsr() & 0x2) SETB(IWN_GIO, IWN_GIO_L0S_ENA);   /* ASPM L1 entry enabled */
    else CLRB(IWN_GIO, IWN_GIO_L0S_ENA);
    if (clock_wait() || nic_lock()) return -1;
    prph_write(IWN_APMG_CLK_EN, IWN_APMG_CLK_CTRL_DMA_CLK_RQT);
    DELAY(20);
    prph_setbits(IWN_APMG_PCI_STT, IWN_APMG_PCI_STT_L1A_DIS);
    nic_unlock();
    return 0;
}

static void apm_stop(void) {
    SETB(IWN_RESET, IWN_RESET_STOP_MASTER);
    for (int i = 0; i < 100; i++) {
        if (RD(IWN_RESET) & IWN_RESET_MASTER_DISABLED) break;
        DELAY(10);
    }
    SETB(IWN_RESET, IWN_RESET_SW);
    DELAY(10);
    CLRB(IWN_GP_CNTRL, IWN_GP_CNTRL_INIT_DONE);
}

static int nic_config(void) {
    if (IWN_RFCFG_TYPE(sc.rfcfg) < 3)
        SETB(IWN_HW_IF_CONFIG, IWN_RFCFG_TYPE(sc.rfcfg) | IWN_RFCFG_STEP(sc.rfcfg) | IWN_RFCFG_DASH(sc.rfcfg));
    SETB(IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_RADIO_SI | IWN_HW_IF_CONFIG_MAC_SI);
    if (nic_lock()) return -1;
    prph_setbits(IWN_APMG_PS, IWN_APMG_PS_EARLY_PWROFF_DIS);
    nic_unlock();
    if (sc.calib_ver >= 6) SETB(IWN_GP_DRIVER, IWN_GP_DRIVER_CALIB_VER6);
    SETB(IWN_GP_DRIVER, IWN_GP_DRIVER_6050_1X2);
    return 0;
}

static int hw_prepare(void) {
    SETB(IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_NIC_READY);
    for (int i = 0; i < 5; i++) { if (RD(IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_NIC_READY) return 0; DELAY(10); }
    SETB(IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_PREPARE);
    int n;
    for (n = 0; n < 15000; n++) { if (!(RD(IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_PREPARE_DONE)) break; DELAY(10); }
    if (n == 15000) return -1;
    SETB(IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_NIC_READY);
    for (int i = 0; i < 5; i++) { if (RD(IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_NIC_READY) return 0; DELAY(10); }
    return -1;
}

/* ---- EEPROM contents (iwn_read_eeprom, iwn5000_read_eeprom, iwn_read_eeprom_channels) -- */
static void read_channels(int n, u32 addr) {
    const struct iwn_chan_band *band = &iwn_bands[n];
    struct iwn_eeprom_chan ch[IWN_MAX_CHAN_PER_BAND];
    if (read_prom(addr, ch, band->nchan * (int)sizeof(struct iwn_eeprom_chan))) return;
    for (int i = 0; i < band->nchan; i++) {
        if (!(ch[i].flags & IWN_EEPROM_CHAN_VALID)) continue;
        u8 c = band->chan[i];
        if (n > 0 && c <= 14) continue;               /* 4.9 GHz duplicates of 2 GHz numbers */
        int dup = 0;
        for (int j = 0; j < sc.nchans; j++) if (sc.chans[j] == c) dup = 1;
        if (dup || sc.nchans >= (int)sizeof sc.chans) continue;
        if (n > 0) sc.flags |= F_5GHZ;
        sc.passive[sc.nchans] = !(ch[i].flags & IWN_EEPROM_CHAN_ACTIVE);
        sc.chans[sc.nchans++] = c;
    }
}

static int read_eeprom(void) {
    if (RD(IWN_OTP_GP) & IWN_OTP_GP_DEV_SEL_OTP) sc.flags |= F_OTPROM;
    if (apm_init()) { LOG("could not power on the adapter"); return -1; }
    if ((RD(IWN_EEPROM_GP) & 0x7) == 0) { LOG("bad ROM signature"); return -1; }
    if (eeprom_lock()) { LOG("could not lock the ROM"); return -1; }
    if ((sc.flags & F_OTPROM) && init_otprom()) { LOG("could not initialize the OTPROM"); eeprom_unlock(); return -1; }
    u16 val = 0;
    read_prom(IWN_EEPROM_RFCFG, &val, 2);
    sc.rfcfg = val;
    sc.txchainmask = (u8)IWN_RFCFG_TXANTMSK(sc.rfcfg);
    sc.rxchainmask = (u8)IWN_RFCFG_RXANTMSK(sc.rfcfg);
    read_prom(IWN_EEPROM_MAC, sc.macaddr, 6);

    /* iwn5000_read_eeprom */
    read_prom(IWN5000_EEPROM_REG, &val, 2);
    u32 base = val;
    read_prom(base + IWN5000_EEPROM_DOMAIN, sc.domain, 4);
    sc.domain[4] = 0;
    for (int i = 0; i < 5; i++) read_channels(i, base + iwn5000_regulatory_bands[i]);   /* 20 MHz bands */
    read_prom(IWN5000_EEPROM_CAL, &val, 2);
    base = val;
    struct iwn5000_eeprom_calib_hdr hdr;
    read_prom(base, &hdr, sizeof hdr);
    sc.calib_ver = hdr.version;
    read_prom(base + IWN5000_EEPROM_CRYSTAL, &sc.eeprom_crystal, 4);
    apm_stop();
    eeprom_unlock();
    sc.ntxchains = (u8)(((sc.txchainmask >> 2) & 1) + ((sc.txchainmask >> 1) & 1) + (sc.txchainmask & 1));
    sc.nrxchains = (u8)(((sc.rxchainmask >> 2) & 1) + ((sc.rxchainmask >> 1) & 1) + (sc.rxchainmask & 1));
    return 0;
}

/* ---- the firmware file (iwn_read_firmware_tlv) ----------------------------------------- */
static int read_firmware(void) {
    vnode_t *n = vfs_lookup(FW_PATH);
    if (!n || !n->data || n->size < (u64)sizeof(struct iwn_fw_tlv_hdr)) {
        /* some firmware FAT drivers list the 8.3 names (IWLWIF~1.UCO): find it by its header */
        vnode_t *dir = vfs_lookup("/lib/firmware");
        n = NULL;
        for (int i = 0; dir && vfs_child_at(dir, i); i++) {
            vnode_t *c = vfs_child_at(dir, i);
            if (c->dir || !c->data || c->size < (u64)sizeof(struct iwn_fw_tlv_hdr)) continue;
            if (!memcmp(c->data + 8, "6000g2b fw", 10)) { n = c; LOG("firmware found as /lib/firmware/%s", c->name); break; }
        }
        if (!n) {
#if defined(__x86_64__)
            extern const u8 iwn_fw_blob[], iwn_fw_blob_end[];
            static vnode_t builtin;                           /* the copy linked into the kernel (fw.S) */
            builtin.data = (u8 *)iwn_fw_blob;
            builtin.size = (u64)(iwn_fw_blob_end - iwn_fw_blob);
            if (builtin.size >= (u64)sizeof(struct iwn_fw_tlv_hdr) && !memcmp(iwn_fw_blob + 8, "6000g2b fw", 10)) {
                n = &builtin;
                LOG("firmware: using the copy built into the kernel (%llu bytes)", builtin.size);
            }
        }
        if (!n) {
#endif
            char names[200] = "";
            for (int i = 0; dir && vfs_child_at(dir, i); i++) {
                vnode_t *c = vfs_child_at(dir, i);
                if (strlen(names) + strlen(c->name) + 12 < sizeof names) {
                    char one[48];
                    fmt(one, sizeof one, " %s(%llu)", c->name, c->size);
                    strlcat(names, one, sizeof names);
                }
            }
            LOG("firmware %s not found on the boot stick; /lib/firmware has:%s", FW_PATH, dir ? names : " (no such directory)");
            return -1;
        }
    }
    sc.fw = n->data;
    sc.fw_len = (usize)n->size;
    const struct iwn_fw_tlv_hdr *h = (const void *)sc.fw;
    if (*(const u32 *)sc.fw != 0 || h->signature != IWN_FW_SIGNATURE) { LOG("not a TLV firmware file"); return -1; }
    fmt(sc.fwver, sizeof sc.fwver, "%u.%u.%u.%u", (h->rev >> 24) & 0xff, (h->rev >> 16) & 0xff, (h->rev >> 8) & 0xff, h->rev & 0xff);
    sc.reset_noise_gain = IWN5000_PHY_CALIB_RESET_NOISE_GAIN;
    sc.noise_gain = IWN5000_PHY_CALIB_NOISE_GAIN;
    u64 altmask = h->altmask;
    u16 alt = 1;
    while (alt > 0 && !(altmask & (1ULL << alt))) alt--;
    const u8 *p = (const u8 *)(h + 1), *end = sc.fw + sc.fw_len;
    memset(&sc.main, 0, sizeof sc.main);
    memset(&sc.init, 0, sizeof sc.init);
    while (p + sizeof(struct iwn_fw_tlv) <= end) {
        const struct iwn_fw_tlv *t = (const void *)p;
        u32 len = t->len;
        p += sizeof *t;
        if (p + len > end) { LOG("firmware truncated"); return -1; }
        if (!(t->alt != 0 && t->alt != alt)) {
            switch (t->type) {
            case IWN_FW_TLV_MAIN_TEXT: sc.main.text = p; sc.main.textsz = len; break;
            case IWN_FW_TLV_MAIN_DATA: sc.main.data = p; sc.main.datasz = len; break;
            case IWN_FW_TLV_INIT_TEXT: sc.init.text = p; sc.init.textsz = len; break;
            case IWN_FW_TLV_INIT_DATA: sc.init.data = p; sc.init.datasz = len; break;
            case IWN_FW_TLV_ENH_SENS: if (len == 0) sc.flags |= F_ENH_SENS; break;
            case IWN_FW_TLV_PHY_CALIB:
                if (len == 4 && *(const u32 *)p <= IWN5000_PHY_CALIB_MAX) { sc.reset_noise_gain = *(const u32 *)p; sc.noise_gain = *(const u32 *)p + 1; }
                break;
            case IWN_FW_TLV_FLAGS: if (len >= 4 && !(len % 4)) sc.tlv_feature_flags = *(const u32 *)p; break;
            }
        }
        p += (len + 3) & ~3u;
    }
    if (!sc.main.text || !sc.init.text || sc.main.textsz > IWN5000_FW_TEXT_MAXSZ || sc.init.textsz > IWN5000_FW_TEXT_MAXSZ ||
        sc.main.datasz > IWN5000_FW_DATA_MAXSZ || sc.init.datasz > IWN5000_FW_DATA_MAXSZ) {
        LOG("firmware sections missing or too large"); return -1;
    }
    return 0;
}

/* ---- rings (iwn_alloc_*_ring, iwn_reset_*_ring) -------------------------------------- */
static int alloc_rings(void) {
    sc.fw_dma = dma_new(IWN5000_FWSZ);
    sc.kw = dma_new(4096);
    sc.sched = dma_new((IWN5000_SCHEDSZ + 1023) & ~1023u);
    for (int q = 0; q < NTXQ; q++) {
        sc.txq[q].qid = q;
        sc.txq[q].desc = dma_new(IWN_TX_RING_COUNT * sizeof(struct iwn_tx_desc));
        sc.txq[q].slots = dma_new(q == CMD_QUEUE || q == DATA_QUEUE ? IWN_TX_RING_COUNT * SLOT : 4096);
    }
    sc.rxq.desc = dma_new(IWN_RX_RING_COUNT * 4);
    sc.rxq.stat = dma_new(4096);
    sc.rxq.bufs = dma_new((usize)IWN_RX_RING_COUNT * IWN_RBUF_SIZE);
    if (!sc.fw_dma.va || !sc.kw.va || !sc.sched.va || !sc.rxq.desc.va || !sc.rxq.stat.va || !sc.rxq.bufs.va) return -1;
    for (int q = 0; q < NTXQ; q++) if (!sc.txq[q].desc.va || !sc.txq[q].slots.va) return -1;
    u32 *d = (u32 *)sc.rxq.desc.va;
    for (int i = 0; i < IWN_RX_RING_COUNT; i++) d[i] = (u32)((sc.rxq.bufs.pa + (u64)i * IWN_RBUF_SIZE) >> 8);
    return 0;
}

static void reset_rings(void) {
    if (!nic_lock()) {
        WR(IWN_FH_RX_CONFIG, 0);
        for (int i = 0; i < 1000; i++) { if (RD(IWN_FH_RX_STATUS) & IWN_FH_RX_STATUS_IDLE) break; DELAY(10); }
        nic_unlock();
    }
    sc.rxq.cur = 0;
    sc.last_rx_valid = 0;
    for (int q = 0; q < NTXQ; q++) {
        memset(sc.txq[q].desc.va, 0, sc.txq[q].desc.size);
        memset(sc.txq[q].used, 0, sizeof sc.txq[q].used);
        sc.txq[q].cur = sc.txq[q].queued = 0;
    }
    memset(sc.sched.va, 0, sc.sched.size);
    rxq_head = rxq_count = 0;
}

/* iwn5000_update_sched, iwn5000_reset_sched */
static void update_sched(int qid, int idx, u8 id, u16 len) {
    u16 *w = (u16 *)sc.sched.va + qid * IWN5000_SCHED_COUNT + idx;
    *w = (u16)(id << 12 | (len + 8));
    if (idx < IWN_SCHED_WINSZ) *(w + IWN_TX_RING_COUNT) = *w;
}
static void reset_sched(int qid, int idx) {
    u16 *w = (u16 *)sc.sched.va + qid * IWN5000_SCHED_COUNT + idx;
    *w = (u16)((*w & 0xf000) | 1);
    if (idx < IWN_SCHED_WINSZ) *(w + IWN_TX_RING_COUNT) = *w;
}

/* ---- the interrupt handler, polled (iwn_intr, iwn_notif_intr) ------------------------- */
static void fatal_dump(void) {
    if (sc.errptr < IWN_FW_DATA_BASE || sc.errptr + sizeof(struct iwn_fw_dump) > IWN_FW_DATA_BASE + IWN5000_FW_DATA_MAXSZ) {
        LOG("fatal firmware error (no error log)"); return;
    }
    if (nic_lock()) return;
    struct iwn_fw_dump dump;
    u32 *d = (u32 *)&dump;
    for (u32 i = 0; i < sizeof dump / 4; i++) d[i] = mem_read(sc.errptr + i * 4);
    nic_unlock();
    LOG("fatal firmware error: %s (0x%08x), pc 0x%08x, data 0x%08x%08x",
        dump.id < sizeof iwn_fw_errmsg / sizeof iwn_fw_errmsg[0] ? iwn_fw_errmsg[dump.id] : "?", dump.id, dump.pc,
        dump.error_data[0], dump.error_data[1]);
}

static void rx_calib_result(const struct iwn_rx_desc *desc) {
    const struct iwn_phy_calib *c = (const void *)(desc + 1);
    int len = (int)(desc->len & IWN_RX_DESC_LEN_MASK) - 4, idx = -1;
    if (sc.flags & F_CALIB_DONE || len <= 0 || len > 2048) return;
    switch (c->code) {
    case IWN5000_PHY_CALIB_LO: idx = 1; break;
    case IWN5000_PHY_CALIB_TX_IQ: idx = 2; break;
    case IWN5000_PHY_CALIB_BASE_BAND: idx = 4; break;
    }
    if (idx < 0) return;
    if (sc.calib[idx].buf) kfree(sc.calib[idx].buf);
    sc.calib[idx].buf = kalloc((usize)len);
    memcpy(sc.calib[idx].buf, c, (usize)len);
    sc.calib[idx].len = len;
}

static void rx_frame(const struct iwn_rx_desc *desc) {
    if (!(sc.last_rx_valid)) return;                    /* each MPDU follows its RX_PHY */
    sc.last_rx_valid = 0;
    const struct iwn_rx_stat *stat = &sc.last_rx_stat;
    if (stat->cfg_phy_len > IWN_STAT_MAXLEN) return;
    const struct iwn_rx_mpdu *mpdu = (const void *)(desc + 1);
    u32 len = mpdu->len;
    const u8 *frame = (const u8 *)(mpdu + 1);
    if (len < 24 || len + sizeof *desc + sizeof *mpdu + 4 > IWN_RBUF_SIZE) return;
    u32 flags = *(const u32 *)(frame + len);
    if ((flags & IWN_RX_NOERROR) != IWN_RX_NOERROR) return;
    iwm_rxinfo_t ri = { 0 };
    ri.channel = stat->chan;
    ri.rstamp = (u32)stat->tstamp;
    const struct iwn5000_rx_phystat *phy = (const void *)stat->phybuf;
    int agc = (int)((phy->agc >> 9) & 0x7f);
    int rssi = MAX(phy->rssi[0] & 0xff, phy->rssi[1] & 0xff);
    rssi = MAX(phy->rssi[2] & 0xff, rssi);
    ri.rssi = rssi - agc - IWN_RSSI_TO_DBM;
    /* pairwise CCMP decrypted by the firmware (iwn_rx_done, iwn_ccmp_decap): the MIC goes */
    if ((frame[1] & 0x40) && !(frame[4] & 1) && sc.have_ptk) {
        if ((flags & IWN_RX_CIPHER_MASK) != IWN_RX_CIPHER_CCMP ||
            (flags & (IWN_RX_MPDU_DEC | IWN_RX_MPDU_MIC_OK)) != (IWN_RX_MPDU_DEC | IWN_RX_MPDU_MIC_OK)) return;
        if (len < 24 + 8 + 8) return;
        len -= 8;
        ri.decrypted = 1;
    }
    sc.rx_frames++;
    if (!sc.rx) return;
    if (rxq_count == RXQ_N || len > RXQ_FRAME) { sc.rx_dropped++; return; }
    int slot = (rxq_head + rxq_count) % RXQ_N;
    rxq_buf[slot].len = (u16)len;
    rxq_buf[slot].ri = ri;
    memcpy(rxq_buf[slot].frame, frame, len);
    rxq_count++;
}

static int start_scan(int five);

static void notif(void) {
    static int busy;
    if (busy) return;
    const struct iwn_rx_status *st = (const void *)sc.rxq.stat.va;
    barrier();
    int hw = st->closed_count & 0xfff & (IWN_RX_RING_COUNT - 1);
    if (hw == sc.rxq.cur) return;
    busy = 1;
    while (sc.rxq.cur != hw) {
        int cur = sc.rxq.cur;
        sc.rxq.cur = (cur + 1) % IWN_RX_RING_COUNT;
        const struct iwn_rx_desc *desc = (const void *)(sc.rxq.bufs.va + (usize)cur * IWN_RBUF_SIZE);
        if (!(desc->qid & 0x80) && (desc->qid & 0xf) == CMD_QUEUE) sc.cmd_done[desc->idx] = 1;   /* iwn_cmd_done */
        switch (desc->type) {
        case IWN_RX_PHY:
            memcpy(&sc.last_rx_stat, desc + 1, sizeof sc.last_rx_stat);
            sc.last_rx_valid = 1;
            break;
        case IWN_MPDU_RX_DONE:
            rx_frame(desc);
            break;
        case IWN_TX_DONE: {                                         /* iwn5000_tx_done */
            const struct iwn5000_tx_stat *ts = (const void *)(desc + 1);
            int qid = desc->qid & 0x1f, idx = desc->idx;
            if (qid >= NTXQ) break;
            txring_t *r = &sc.txq[qid];
            u8 status = (u8)(ts->stat.status & 0xff);
            if (status != IWN_TX_STATUS_SUCCESS && status != IWN_TX_STATUS_DIRECT_DONE) sc.tx_fail++;
            reset_sched(qid, idx);
            if (r->used[idx]) { r->used[idx] = 0; r->queued--; }
            break;
        }
        case IWN_BEACON_MISSED: {
            const struct iwn_beacon_missed *m = (const void *)(desc + 1);
            sc.missed_beacons = m->consecutive;
            break;
        }
        case IWN_UC_READY: {
            const struct iwn_ucode_info *uc = (const void *)(desc + 1);
            if (uc->valid != 1) { LOG("microcontroller initialization failed"); break; }
            sc.errptr = uc->errptr;
            sc.alive = 1;
            break;
        }
        case IWN_STATE_CHANGED: {
            u32 s = *(const u32 *)(desc + 1);
            if (s & 1) LOG("radio transmitter switched off (rfkill)");
            break;
        }
        case IWN_STOP_SCAN: {
            const struct iwn_stop_scan *ss = (const void *)(desc + 1);
            /* the 2 GHz channels are done: the 5 GHz ones next (iwn_notif_intr) */
            if (ss->status == 1 && ss->chan <= 14 && (sc.flags & F_5GHZ) && !sc.scan_5ghz) {
                sc.scan_5ghz = 1;
                if (start_scan(1) == 0) break;
            }
            sc.scanning = 0;
            break;
        }
        case IWN5000_CALIBRATION_RESULT: rx_calib_result(desc); break;
        case IWN5000_CALIBRATION_DONE: sc.flags |= F_CALIB_DONE; sc.calib_done = 1; break;
        }
    }
    busy = 0;
    int w = hw == 0 ? IWN_RX_RING_COUNT - 1 : hw - 1;
    WR(IWN_FH_RX_WPTR, (u32)(w & ~7));
}

static void service(void) {
    if (!sc.regs) return;
    u32 r1 = RD(IWN_INT);
    if (r1 == 0xffffffffu || (r1 & 0xfffffff0u) == 0xa5a5a5a0u) {
        if (!sc.fatal) LOG("device stopped answering (INT %08x)", r1);
        sc.fatal = 1;
        return;
    }
    u32 r2 = RD(IWN_FH_INT);
    if (r1) WR(IWN_INT, r1);
    if (r2) WR(IWN_FH_INT, r2);
    if (r1 & (IWN_INT_SW_ERR | IWN_INT_HW_ERR)) {
        if (!sc.fatal) fatal_dump();
        sc.fatal = 1;
        sc.flags &= ~F_CALIB_DONE;                          /* recalibrate on the next start */
        return;
    }
    if (r1 & IWN_INT_RF_TOGGLED) LOG("radio switch: %s", (RD(IWN_GP_CNTRL) & IWN_GP_CNTRL_RFKILL) ? "on" : "off");
    if (r1 & IWN_INT_CT_REACHED) LOG("critical temperature reached");
    if ((r1 & IWN_INT_FH_TX) || (r2 & IWN_FH_INT_TX)) sc.fw_dma_done = 1;
    if (r1 & IWN_INT_ALIVE) sc.alive = 1;
    notif();
}

static int wait_for(volatile int *flag, u32 ms) {
    u64 deadline = k_now_ms() + ms;
    while (k_now_ms() < deadline && !sc.fatal) {
        service();
        if (*flag) return 0;
        DELAY(50);
    }
    return *flag ? 0 : -1;
}

/* ---- commands (iwn_cmd) ---------------------------------------------------------------- */
static int cmd(int code, const void *buf, int size, int async) {
    txring_t *r = &sc.txq[CMD_QUEUE];
    int idx = r->cur, totlen = 4 + size;
    if (totlen > SLOT) return -1;
    u8 *slot = r->slots.va + (usize)idx * SLOT;
    u64 pa = r->slots.pa + (u64)idx * SLOT;
    slot[0] = (u8)code; slot[1] = 0; slot[2] = (u8)idx; slot[3] = (u8)r->qid;   /* code, flags, idx, qid */
    memcpy(slot + 4, buf, (usize)size);
    struct iwn_tx_desc *desc = (struct iwn_tx_desc *)r->desc.va + idx;
    memset(desc, 0, sizeof *desc);
    desc->nsegs = 1;
    desc->segs[0].addr = IWN_LOADDR(pa);
    desc->segs[0].len = (u16)(IWN_HIADDR(pa) | (u32)totlen << 4);
    update_sched(r->qid, idx, 0, 0);
    sc.cmd_done[idx] = 0;
    barrier();
    r->cur = (idx + 1) % IWN_TX_RING_COUNT;
    WR(IWN_HBUS_TARG_WRPTR, (u32)(r->qid << 8 | r->cur));
    if (async) return 0;
    u64 deadline = k_now_ms() + 1000;
    while (k_now_ms() < deadline && !sc.fatal) {
        service();
        if (sc.cmd_done[idx]) return 0;
        DELAY(20);
    }
    LOG("command %d: no answer", code);
    return -1;
}

/* ---- firmware loading (iwn5000_load_firmware_section, iwn5000_load_firmware) ---------- */
static int load_section(u32 dst, const u8 *section, u32 size) {
    memcpy(sc.fw_dma.va, section, size);
    barrier();
    if (nic_lock()) return -1;
    WR(IWN_FH_TX_CONFIG(IWN_SRVC_DMACHNL), IWN_FH_TX_CONFIG_DMA_PAUSE);
    WR(IWN_FH_SRAM_ADDR(IWN_SRVC_DMACHNL), dst);
    WR(IWN_FH_TFBD_CTRL0(IWN_SRVC_DMACHNL), IWN_LOADDR(sc.fw_dma.pa));
    WR(IWN_FH_TFBD_CTRL1(IWN_SRVC_DMACHNL), IWN_HIADDR(sc.fw_dma.pa) << 28 | size);
    WR(IWN_FH_TXBUF_STATUS(IWN_SRVC_DMACHNL), IWN_FH_TXBUF_STATUS_TBNUM(1) | IWN_FH_TXBUF_STATUS_TBIDX(1) | IWN_FH_TXBUF_STATUS_TFBD_VALID);
    sc.fw_dma_done = 0;
    WR(IWN_FH_TX_CONFIG(IWN_SRVC_DMACHNL), IWN_FH_TX_CONFIG_DMA_ENA | IWN_FH_TX_CONFIG_CIRQ_HOST_ENDTFD);
    nic_unlock();
    return wait_for(&sc.fw_dma_done, 5000);
}

static int load_firmware(void) {
    int runtime = (sc.flags & F_CALIB_DONE) != 0;
    const u8 *text = runtime ? sc.main.text : sc.init.text, *data = runtime ? sc.main.data : sc.init.data;
    u32 ts = runtime ? sc.main.textsz : sc.init.textsz, ds = runtime ? sc.main.datasz : sc.init.datasz;
    if (load_section(IWN_FW_TEXT_BASE, text, ts)) { LOG("could not load the firmware's .text"); return -1; }
    if (load_section(IWN_FW_DATA_BASE, data, ds)) { LOG("could not load the firmware's .data"); return -1; }
    WR(IWN_RESET, 0);                                       /* now press "execute" */
    return 0;
}

/* ---- calibration and setup commands (iwn5000_*_calib, iwn_send_*btcoex, ...) ----------- */
static int send_wimax_coex(void) {
    struct iwn5000_wimax_coex w;
    memset(&w, 0, sizeof w);                                /* coexistence off */
    return cmd(IWN5000_CMD_WIMAX_COEX, &w, sizeof w, 0);
}
static int crystal_calib(void) {
    struct iwn5000_phy_calib_crystal c;
    memset(&c, 0, sizeof c);
    c.code = IWN5000_PHY_CALIB_CRYSTAL;
    c.ngroups = 1; c.isvalid = 1;
    c.cap_pin[0] = (u8)(sc.eeprom_crystal & 0xff);
    c.cap_pin[1] = (u8)((sc.eeprom_crystal >> 16) & 0xff);
    return cmd(IWN_CMD_PHY_CALIB, &c, sizeof c, 0);
}
static int query_calibration(void) {
    struct iwn5000_calib_config c;
    memset(&c, 0, sizeof c);
    c.ucode.once.enable = 0xffffffff;
    c.ucode.once.start = 0xffffffff;
    c.ucode.once.send = 0xffffffff;
    c.ucode.flags = 0xffffffff;
    sc.calib_done = 0;
    if (cmd(IWN5000_CMD_CALIB_CONFIG, &c, sizeof c, 0)) return -1;
    if (!(sc.flags & F_CALIB_DONE) && wait_for(&sc.calib_done, 2000)) { LOG("calibration did not complete"); return -1; }
    return 0;
}
static int send_calibration(void) {
    for (int i = 0; i < 5; i++)
        if (sc.calib[i].buf && cmd(IWN_CMD_PHY_CALIB, sc.calib[i].buf, sc.calib[i].len, 0)) { LOG("could not send a calibration result"); return -1; }
    return 0;
}
static int temp_offset_calib(void) {
    struct iwn6000_phy_calib_temp_offset c;
    memset(&c, 0, sizeof c);
    c.code = IWN6000_PHY_CALIB_TEMP_OFFSET;
    c.ngroups = 1; c.isvalid = 1;
    c.offset = IWN_DEFAULT_TEMP_OFFSET;
    return cmd(IWN_CMD_PHY_CALIB, &c, sizeof c, 0);
}
static int runtime_calib(void) {
    struct iwn5000_calib_config c;
    memset(&c, 0, sizeof c);
    c.ucode.once.enable = 0xffffffff;
    c.ucode.once.start = IWN5000_CALIB_DC;
    return cmd(IWN5000_CMD_CALIB_CONFIG, &c, sizeof c, 0);
}
static int send_advanced_btcoex(void) {
    static const u32 btcoex_3wire[12] = {
        0xaaaaaaaa, 0xaaaaaaaa, 0xaeaaaaaa, 0xaaaaaaaa, 0xcc00ff28, 0x0000aaaa,
        0xcc00aaaa, 0x0000aaaa, 0xc0004000, 0x00004000, 0xf0005000, 0xf0005000,
    };
    struct iwn6000_btcoex_config bt;
    memset(&bt, 0, sizeof bt);
    bt.flags = IWN_BT_COEX6000_CHAN_INHIBITION | (IWN_BT_COEX6000_MODE_3W << IWN_BT_COEX6000_MODE_SHIFT) | IWN_BT_SYNC_2_BT_DISABLE;
    bt.max_kill = 5; bt.bt3_t7_timer = 1;
    bt.kill_ack = 0xffff0000; bt.kill_cts = 0xffff0000;
    bt.sample_time = 2; bt.bt3_t2_timer = 0xc;
    for (int i = 0; i < 12; i++) bt.lookup_table[i] = btcoex_3wire[i];
    bt.valid = 0xff; bt.prio_boost = 0xf0;
    if (cmd(IWN_CMD_BT_COEX, &bt, sizeof bt, 1)) return -1;
    struct iwn_btcoex_priotable prio;
    memset(&prio, 0, sizeof prio);
    prio.calib_init1 = 0x6; prio.calib_init2 = 0x7;
    prio.calib_periodic_low1 = 0x2; prio.calib_periodic_low2 = 0x3;
    prio.calib_periodic_high1 = 0x4; prio.calib_periodic_high2 = 0x5;
    prio.dtim = 0x6; prio.scan52 = 0x8; prio.scan24 = 0xa;
    if (cmd(IWN_CMD_BT_COEX_PRIOTABLE, &prio, sizeof prio, 1)) return -1;
    struct iwn_btcoex_prot prot;
    memset(&prot, 0, sizeof prot);
    prot.open = 1; prot.type = 1;
    if (cmd(IWN_CMD_BT_COEX_PROT, &prot, sizeof prot, 1)) return -1;
    prot.open = 0;
    return cmd(IWN_CMD_BT_COEX_PROT, &prot, sizeof prot, 1);
}
static int set_txpower(int async) {                          /* iwn5000_set_txpower */
    struct iwn5000_cmd_txpower c;
    memset(&c, 0, sizeof c);
    c.global_limit = 2 * IWN5000_TXPOWER_MAX_DBM;
    c.flags = IWN5000_TXPOWER_NO_CLOSED;
    c.srv_limit = IWN5000_TXPOWER_AUTO;
    return cmd(IWN_CMD_TXPOWER_DBM, &c, sizeof c, async);
}
static int set_critical_temp(void) {
    WR(IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_CTEMP_STOP_RF);
    struct iwn_critical_temp c;
    memset(&c, 0, sizeof c);
    c.tempR = 110;
    return cmd(IWN_CMD_SET_CRITICAL_TEMP, &c, sizeof c, 0);
}
static int set_pslevel_cam(void) {                          /* iwn_set_pslevel(sc, 0, 0, 0) */
    const struct iwn_pmgt *pm = &iwn_pmgt[0][0];
    struct iwn_pmgt_cmd c;
    memset(&c, 0, sizeof c);
    if (!(pcie_lcsr() & 0x1)) c.flags |= IWN_PS_PCI_PMGT;     /* L0s entry disabled */
    c.rxtimeout = pm->rxtimeout * 1024;
    c.txtimeout = pm->txtimeout * 1024;
    for (int i = 0; i < 5; i++) c.intval[i] = MIN(1u, pm->intval[i]);
    return cmd(IWN_CMD_SET_POWER_MODE, &c, sizeof c, 0);
}

/* the rate table index for a rate in 500 kb/s units (iwn_rval2ridx) */
static int rval2ridx(int rval) {
    for (int r = 0; r <= 11; r++) if (iwn_rates[r].rate == rval) return r;
    return IWN_RIDX_CCK1;
}

static int add_broadcast_node(int async, int ridx) {        /* iwn_add_broadcast_node */
    struct iwn_node_info node;
    memset(&node, 0, sizeof node);
    memset(node.macaddr, 0xff, 6);
    node.id = IWN5000_ID_BROADCAST;
    if (cmd(IWN_CMD_ADD_NODE, &node, sizeof node, async)) return -1;
    u8 txant = (u8)IWN_LSB(sc.txchainmask);
    struct iwn_cmd_link_quality lq;
    memset(&lq, 0, sizeof lq);
    lq.id = IWN5000_ID_BROADCAST;
    lq.antmsk_1stream = txant;
    lq.antmsk_2stream = IWN_ANT_AB;
    lq.ampdu_max = IWN_AMPDU_MAX_NO_AGG;
    lq.ampdu_threshold = 3;
    lq.ampdu_limit = 4000;
    for (int i = 0; i < IWN_MAX_TX_RETRIES; i++) {
        lq.retry[i].plcp = iwn_rates[ridx].plcp;
        lq.retry[i].rflags = (u8)(iwn_rates[ridx].flags | IWN_RFLAG_ANT(txant));
    }
    return cmd(IWN_CMD_LINK_QUALITY, &lq, sizeof lq, async);
}

/* iwn_config: what the runtime firmware needs before it can scan */
static int config(void) {
    if (sc.hw_type == IWN_HW_REV_TYPE_6005) {
        if (temp_offset_calib()) { LOG("could not set the temperature offset"); return -1; }
        if (runtime_calib()) { LOG("could not configure runtime calibration"); return -1; }
    }
    u32 txmask = sc.txchainmask;
    if (cmd(IWN5000_CMD_TX_ANT_CONFIG, &txmask, sizeof txmask, 0)) { LOG("could not configure TX chains"); return -1; }
    if (send_advanced_btcoex()) { LOG("could not configure Bluetooth coexistence"); return -1; }
    memset(&sc.rxon, 0, sizeof sc.rxon);
    memcpy(sc.rxon.myaddr, sc.macaddr, 6);
    memcpy(sc.rxon.wlap, sc.macaddr, 6);
    sc.rxon.chan = sc.nchans ? sc.chans[0] : 1;
    sc.rxon.flags = IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF | IWN_RXON_AUTO | IWN_RXON_24GHZ;
    sc.rxon.mode = IWN_MODE_STA;
    sc.rxon.filter = IWN_FILTER_MULTICAST;
    sc.rxon.cck_mask = 0x0f;
    sc.rxon.ofdm_mask = 0xff;
    sc.rxon.ht_single_mask = sc.rxon.ht_dual_mask = sc.rxon.ht_triple_mask = 0xff;
    sc.rxon.rxchain = (u16)(IWN_RXCHAIN_VALID(sc.rxchainmask) | IWN_RXCHAIN_MIMO_COUNT(sc.nrxchains) | IWN_RXCHAIN_IDLE_COUNT(sc.nrxchains));
    if (cmd(IWN_CMD_RXON, &sc.rxon, IWN5000_RXONSZ, 0)) { LOG("RXON failed"); return -1; }
    if (add_broadcast_node(0, IWN_RIDX_CCK1)) { LOG("could not add the broadcast node"); return -1; }
    if (set_txpower(0)) { LOG("could not set the TX power"); return -1; }
    if (set_critical_temp()) { LOG("could not set the critical temperature"); return -1; }
    if (set_pslevel_cam()) { LOG("could not set the power mode"); return -1; }
    return 0;
}

/* ---- bring-up (iwn_hw_init, iwn5000_post_alive, iwn_hw_stop) -------------------------- */
static int hw_init(void);
static void hw_stop(void);

static int post_alive(void) {
    if (nic_lock()) return -1;
    u32 sched_base = prph_read(IWN_SCHED_SRAM_ADDR);
    for (u32 i = 0; i < IWN5000_SCHED_CTX_LEN / 4; i++) mem_write(sched_base + IWN5000_SCHED_CTX_OFF + i * 4, 0);
    prph_write(IWN5000_SCHED_DRAM_ADDR, (u32)(sc.sched.pa >> 10));
    prph_write(IWN5000_SCHED_CHAINEXT_EN, 0);
    SETB(IWN_FH_TX_CHICKEN, IWN_FH_TX_CHICKEN_SCHED_RETRY);
    prph_write(IWN5000_SCHED_QCHAIN_SEL, 0xfffef);
    prph_write(IWN5000_SCHED_AGGR_SEL, 0);
    for (int q = 0; q < IWN5000_NTXQUEUES; q++) {
        prph_write(IWN5000_SCHED_QUEUE_RDPTR(q), 0);
        WR(IWN_HBUS_TARG_WRPTR, (u32)(q << 8));
        mem_write(sched_base + IWN5000_SCHED_QUEUE_OFFSET(q), 0);
        mem_write(sched_base + IWN5000_SCHED_QUEUE_OFFSET(q) + 4, IWN_SCHED_LIMIT << 16 | IWN_SCHED_WINSZ);
    }
    prph_write(IWN5000_SCHED_INTR_MASK, 0xfffff);
    prph_write(IWN5000_SCHED_TXFACT, 0xff);
    static const u8 qid2fifo[] = { 3, 2, 1, 0, 7, 5, 6 };
    for (int q = 0; q < 7; q++) prph_write(IWN5000_SCHED_QUEUE_STATUS(q), IWN5000_TXQ_STATUS_ACTIVE | qid2fifo[q]);
    nic_unlock();

    if (send_wimax_coex()) { LOG("could not configure WiMAX coexistence"); return -1; }
    if (crystal_calib()) { LOG("crystal calibration failed"); return -1; }
    if (!(sc.flags & F_CALIB_DONE)) {
        /* the initialization firmware's results, then the runtime firmware */
        if (query_calibration()) return -1;
        hw_stop();
        return hw_init();
    }
    return send_calibration();
}

static int hw_init(void) {
    WR(IWN_INT, 0xffffffff);
    if (apm_init()) { LOG("could not power on the adapter"); return -1; }
    if (nic_lock()) return -1;
    prph_clrbits(IWN_APMG_PS, IWN_APMG_PS_PWR_SRC_MASK);     /* VMAIN */
    nic_unlock();
    if (nic_config()) return -1;

    if (nic_lock()) return -1;
    WR(IWN_FH_RX_CONFIG, 0);
    WR(IWN_FH_RX_WPTR, 0);
    WR(IWN_FH_RX_BASE, (u32)(sc.rxq.desc.pa >> 8));
    WR(IWN_FH_STATUS_WPTR, (u32)(sc.rxq.stat.pa >> 4));
    WR(IWN_FH_RX_CONFIG, IWN_FH_RX_CONFIG_ENA | IWN_FH_RX_CONFIG_IGN_RXF_EMPTY | IWN_FH_RX_CONFIG_IRQ_DST_HOST |
       IWN_FH_RX_CONFIG_SINGLE_FRAME | IWN_FH_RX_CONFIG_RB_TIMEOUT(0x11) | IWN_FH_RX_CONFIG_NRBD(IWN_RX_RING_COUNT_LOG));
    nic_unlock();
    WR(IWN_FH_RX_WPTR, (IWN_RX_RING_COUNT - 1) & ~7);

    if (nic_lock()) return -1;
    prph_write(IWN5000_SCHED_TXFACT, 0);
    WR(IWN_FH_KW_ADDR, (u32)(sc.kw.pa >> 4));
    for (int q = 0; q < NTXQ; q++) WR(IWN_FH_CBBC_QUEUE(q), (u32)(sc.txq[q].desc.pa >> 8));
    nic_unlock();
    for (int ch = 0; ch < IWN5000_NDMACHNLS; ch++) WR(IWN_FH_TX_CONFIG(ch), IWN_FH_TX_CONFIG_DMA_ENA | IWN_FH_TX_CONFIG_DMA_CREDIT_ENA);

    WR(IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
    WR(IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_CMD_BLOCKED);
    WR(IWN_INT, 0xffffffff);
    WR(IWN_INT_COALESCING, 512 / 8);
    WR(IWN_INT_MASK, IWN_INT_MASK_DEF);                       /* causes latch; INTx is off, nothing interrupts */
    WR(IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
    WR(IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
    SETB(IWN_SHADOW_REG_CTRL, 0x800fffff);

    sc.alive = 0;
    if (load_firmware()) return -1;
    if (wait_for(&sc.alive, 1000)) { LOG("timeout waiting for the %s firmware", (sc.flags & F_CALIB_DONE) ? "runtime" : "initialization"); return -1; }
    return post_alive();
}

static void hw_stop(void) {
    WR(IWN_RESET, IWN_RESET_NEVO);
    WR(IWN_INT_MASK, 0);
    WR(IWN_INT, 0xffffffff);
    WR(IWN_FH_INT, 0xffffffff);
    sc.nic_locks = 1; nic_unlock();                          /* no lock held any more */
    prph_write(IWN5000_SCHED_TXFACT, 0);
    if (!nic_lock()) {
        for (int ch = 0; ch < IWN5000_NDMACHNLS; ch++) {
            WR(IWN_FH_TX_CONFIG(ch), 0);
            for (int n = 0; n < 200; n++) { if (RD(IWN_FH_TX_STATUS) & IWN_FH_TX_STATUS_IDLE(ch)) break; DELAY(10); }
        }
        nic_unlock();
    }
    reset_rings();
    if (!nic_lock()) { prph_write(IWN_APMG_CLK_DIS, IWN_APMG_CLK_CTRL_DMA_CLK_RQT); nic_unlock(); }
    DELAY(5);
    apm_stop();
}

/* ---- scanning (iwn_scan) ----------------------------------------------------------------- */
static u8 *add_ie_rates(u8 *p, const u8 *r, int n, int ext) {
    if (!n) return p;
    *p++ = ext ? 50 : 1;
    *p++ = (u8)n;
    memcpy(p, r, (usize)n);
    return p + n;
}

static int start_scan(int five) {
    static u8 buf[IWN_SCAN_MAXSZ];
    memset(buf, 0, sizeof buf);
    struct iwn_scan_hdr *hdr = (void *)buf;
    hdr->quiet_time = 10;
    hdr->quiet_threshold = 1;
    hdr->rxchain = (u16)(IWN_RXCHAIN_VALID(sc.rxchainmask) | IWN_RXCHAIN_FORCE_MIMO_SEL(sc.rxchainmask) |
                         IWN_RXCHAIN_DRIVER_FORCE | IWN_RXCHAIN_FORCE_SEL(sc.rxchainmask));
    hdr->filter = IWN_FILTER_MULTICAST | IWN_FILTER_BEACON;
    struct iwn_cmd_data *tx = (void *)(hdr + 1);
    tx->flags = IWN_TX_AUTO_SEQ;
    tx->id = IWN5000_ID_BROADCAST;
    tx->lifetime = IWN_LIFETIME_INFINITE;
    static const u8 r11g[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 }, r11g_x[] = { 0x30, 0x48, 0x60, 0x6c };
    static const u8 r11a[] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
    if (five) {
        tx->plcp = iwn_rates[IWN_RIDX_OFDM6].plcp;
    } else {
        hdr->flags = IWN_RXON_24GHZ | IWN_RXON_AUTO;
        tx->plcp = iwn_rates[IWN_RIDX_CCK1].plcp;
        tx->rflags = IWN_RFLAG_CCK;
    }
    tx->rflags |= (u8)IWN_RFLAG_ANT(IWN_LSB(sc.txchainmask));
    struct iwn_scan_essid *essid = (void *)(tx + 1);
    u8 *wh = (u8 *)(essid + 20);                             /* a wildcard probe request */
    wh[0] = 0x40;
    memset(wh + 4, 0xff, 6);
    memcpy(wh + 10, sc.macaddr, 6);
    memset(wh + 16, 0xff, 6);
    u8 *frm = wh + 24;
    *frm++ = 0; *frm++ = 0;                                   /* SSID: any */
    if (five) frm = add_ie_rates(frm, r11a, sizeof r11a, 0);
    else { frm = add_ie_rates(frm, r11g, sizeof r11g, 0); frm = add_ie_rates(frm, r11g_x, sizeof r11g_x, 1); }
    tx->len = (u16)(frm - wh);
    hdr->crc_threshold = (sc.tlv_feature_flags & IWN_UCODE_TLV_FLAGS_NEWSCAN) ? IWN_GOOD_CRC_TH_DISABLED : IWN_GOOD_CRC_TH_NEVER;
    struct iwn_scan_chan *chan = (void *)frm;
    for (int i = 0; i < sc.nchans; i++) {
        u8 c = sc.chans[i];
        if ((c > 14) != (five != 0)) continue;
        if ((u8 *)(chan + 1) > buf + sizeof buf) break;
        chan->chan = c;
        chan->flags = sc.passive[i] ? IWN_CHAN_PASSIVE : IWN_CHAN_ACTIVE;
        u16 active = five ? IWN_ACTIVE_DWELL_TIME_5GHZ + IWN_ACTIVE_DWELL_FACTOR_5GHZ : IWN_ACTIVE_DWELL_TIME_2GHZ + IWN_ACTIVE_DWELL_FACTOR_2GHZ;
        u16 passive = IWN_PASSIVE_DWELL_BASE + (five ? IWN_PASSIVE_DWELL_TIME_5GHZ : IWN_PASSIVE_DWELL_TIME_2GHZ);
        if (passive <= active) passive = active + 1;
        chan->active = active;
        chan->passive = passive;
        chan->dsp_gain = 0x6e;
        chan->rf_gain = five ? 0x3b : 0x28;
        hdr->nchan++;
        chan++;
    }
    if (!hdr->nchan) return -1;
    int len = (int)((u8 *)chan - buf);
    hdr->len = (u16)len;
    return cmd(IWN_CMD_SCAN, buf, len, 1);
}

int iwn_scan(void) {
    if (!sc.ready || sc.fatal) return -1;
    if (sc.scanning) return 0;
    sc.scan_5ghz = 0;
    if (start_scan(0)) {
        if (!(sc.flags & F_5GHZ) || start_scan(1)) return -1;
        sc.scan_5ghz = 1;
    }
    sc.scanning = 1;
    return 0;
}
int iwn_scanning(void) { return sc.scanning; }
void iwn_scan_forget(void) { sc.scanning = 0; }
u32 iwn_missed_beacons(void) { return sc.missed_beacons; }

/* ---- association (iwn_auth, iwn_run, iwn_set_key) ----------------------------------------- */
static void rxon_band(const iwm_bss_t *b) {
    sc.rxon.chan = (u8)b->channel;
    sc.rxon.flags = IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF;
    if (b->channel <= 14) sc.rxon.flags |= IWN_RXON_AUTO | IWN_RXON_24GHZ;
    if (b->short_slot) sc.rxon.flags |= IWN_RXON_SHSLOT;
    if (b->short_preamble) sc.rxon.flags |= IWN_RXON_SHPREAMBLE;
    if (b->channel > 14) { sc.rxon.cck_mask = 0; sc.rxon.ofdm_mask = 0x15; }
    else {
        int ofdm = 0;
        for (int i = 0; i < b->n_rates; i++) { int r = b->rates[i] & 0x7f; if (r != 2 && r != 4 && r != 11 && r != 22) ofdm = 1; }
        sc.rxon.cck_mask = ofdm ? 0x0f : 0x03;
        sc.rxon.ofdm_mask = ofdm ? 0x15 : 0;
    }
}

int iwn_auth_prepare(const iwm_bss_t *b) {
    if (!sc.ready || sc.fatal) return -1;
    memcpy(sc.bssid, b->bssid, 6);
    sc.channel = b->channel;
    sc.nrates = MIN(b->n_rates, 16);
    memcpy(sc.rates, b->rates, (usize)sc.nrates);
    memcpy(sc.rxon.bssid, b->bssid, 6);
    sc.rxon.associd = 0;
    sc.rxon.filter = IWN_FILTER_MULTICAST;
    rxon_band(b);
    if (cmd(IWN_CMD_RXON, &sc.rxon, IWN5000_RXONSZ, 0)) { LOG("RXON for %d failed", b->channel); return -1; }
    if (set_txpower(0)) return -1;
    if (add_broadcast_node(0, b->channel > 14 ? IWN_RIDX_OFDM6 : IWN_RIDX_CCK1)) return -1;
    /* the firmware's regulatory checks want a beacon before we transmit */
    u64 until = k_now_ms() + (u64)(b->beacon_int ? b->beacon_int : 100) * 3;
    while (k_now_ms() < until && !sc.fatal) { service(); DELAY(1000); }
    return sc.fatal ? -1 : 0;
}

static int set_timing(const iwm_bss_t *b) {
    struct iwn_cmd_timing t;
    memset(&t, 0, sizeof t);
    t.tstamp = b->tsf;
    u16 intval = b->beacon_int ? b->beacon_int : 100;
    t.bintval = intval;
    t.lintval = 10;
    u64 val = (u64)intval * 1024, mod = b->tsf % val;
    t.binitval = (u32)(val - mod);
    return cmd(IWN_CMD_TIMING, &t, sizeof t, 0);
}

static int set_link_quality(void) {
    u8 txant = (u8)IWN_LSB(sc.txchainmask);
    struct iwn_cmd_link_quality lq;
    memset(&lq, 0, sizeof lq);
    lq.id = IWN_ID_BSS;
    lq.antmsk_1stream = txant;
    lq.antmsk_2stream = IWN_ANT_AB;
    lq.ampdu_max = IWN_AMPDU_MAX;
    lq.ampdu_threshold = 3;
    lq.ampdu_limit = 4000;
    /* the AP's rates, fastest first, each retried down the table; the lowest to fill */
    int r[16], n = 0, lowest = sc.channel > 14 ? IWN_RIDX_OFDM6 : IWN_RIDX_CCK1;
    for (int i = 0; i < sc.nrates; i++) {
        int ridx = rval2ridx(sc.rates[i] & 0x7f);
        if (sc.channel > 14 && ridx < IWN_RIDX_OFDM6) continue;
        int dup = 0;
        for (int j = 0; j < n; j++) if (r[j] == ridx) dup = 1;
        if (!dup && n < 16) r[n++] = ridx;
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (iwn_rates[r[j]].rate > iwn_rates[r[i]].rate) { int t = r[i]; r[i] = r[j]; r[j] = t; }
    for (int i = 0; i < IWN_MAX_TX_RETRIES; i++) {
        int ridx = i < n ? r[i] : lowest;
        lq.retry[i].plcp = iwn_rates[ridx].plcp;
        lq.retry[i].rflags = (u8)(iwn_rates[ridx].flags | IWN_RFLAG_ANT(txant));
    }
    return cmd(IWN_CMD_LINK_QUALITY, &lq, sizeof lq, 0);
}

static int init_sensitivity(void) {                          /* iwn_init_sensitivity, first setting only */
    const struct iwn_sensitivity_limits *l = &iwn6000_sensitivity_limits;
    struct iwn_enhanced_sensitivity_cmd c;
    memset(&c, 0, sizeof c);
    int len = sizeof(struct iwn_sensitivity_cmd);
    c.which = IWN_SENSITIVITY_WORKTBL;
    c.corr_ofdm_x1 = l->min_ofdm_x1; c.corr_ofdm_mrc_x1 = l->min_ofdm_mrc_x1;
    c.corr_ofdm_x4 = l->min_ofdm_x4; c.corr_ofdm_mrc_x4 = l->min_ofdm_mrc_x4;
    c.energy_ofdm = l->energy_ofdm; c.energy_ofdm_th = 62;
    c.corr_cck_x4 = 125; c.corr_cck_mrc_x4 = l->min_cck_mrc_x4; c.energy_cck = l->energy_cck;
    c.corr_barker = 190; c.corr_barker_mrc = 390;
    if (sc.flags & F_ENH_SENS) {
        len = sizeof c;
        c.ofdm_det_slope_mrc = 668; c.ofdm_det_icept_mrc = 4; c.ofdm_det_slope = 486; c.ofdm_det_icept = 37;
        c.cck_det_slope_mrc = 853; c.cck_det_icept_mrc = 4; c.cck_det_slope = 476; c.cck_det_icept = 99;
    }
    if (cmd(IWN_CMD_SET_SENSITIVITY, &c, len, 0)) return -1;
    struct iwn_phy_calib g;                                 /* iwn5000_init_gains */
    memset(&g, 0, sizeof g);
    g.code = (u8)sc.reset_noise_gain; g.ngroups = 1; g.isvalid = 1;
    return cmd(IWN_CMD_PHY_CALIB, &g, sizeof g, 0);
}

int iwn_assoc_done(const iwm_bss_t *b) {
    if (!sc.ready || sc.fatal) return -1;
    if (set_timing(b)) { LOG("could not set timing"); return -1; }
    sc.rxon.associd = (u16)(b->assoc_id & 0x3fff);
    rxon_band(b);
    sc.rxon.filter |= IWN_FILTER_BSS;
    if (cmd(IWN_CMD_RXON, &sc.rxon, IWN5000_RXONSZ, 0)) { LOG("could not update the configuration"); return -1; }
    if (set_txpower(0)) return -1;
    struct iwn_node_info node;
    memset(&node, 0, sizeof node);
    memcpy(node.macaddr, b->bssid, 6);
    node.id = IWN_ID_BSS;
    if (cmd(IWN_CMD_ADD_NODE, &node, sizeof node, 0)) { LOG("could not add the access point"); return -1; }
    if (set_link_quality()) { LOG("could not set link quality"); return -1; }
    if (init_sensitivity()) LOG("could not set the receiver's sensitivity (default kept)");
    sc.associated = 1;
    sc.missed_beacons = 0;
    return 0;
}

void iwn_disconnect(void) {
    sc.have_ptk = 0;
    sc.tsc = 0;
    sc.associated = 0;
    if (!sc.ready || sc.fatal) return;
    memset(sc.rxon.bssid, 0, 6);
    sc.rxon.associd = 0;
    sc.rxon.filter = IWN_FILTER_MULTICAST;
    cmd(IWN_CMD_RXON, &sc.rxon, IWN5000_RXONSZ, 0);           /* clears the node table */
    add_broadcast_node(0, IWN_RIDX_CCK1);
}

int iwn_set_pairwise_key(const u8 key[16]) {
    if (!sc.ready || sc.fatal) return -1;
    struct iwn_node_info node;
    memset(&node, 0, sizeof node);
    node.id = IWN_ID_BSS;
    node.control = IWN_NODE_UPDATE;
    node.flags = IWN_FLAG_SET_KEY;
    node.kflags = IWN_KFLAG_CCMP | IWN_KFLAG_MAP | IWN_KFLAG_KID(0);
    node.kid = 0;
    memcpy(node.key, key, 16);
    if (cmd(IWN_CMD_ADD_NODE, &node, sizeof node, 0)) return -1;
    memcpy(sc.ptk_tk, key, 16);
    sc.have_ptk = 1;
    sc.tsc = 0;
    return 0;
}

/* ---- sending (iwn_tx) ------------------------------------------------------------------ */
int iwn_tx(const u8 *frame, usize len, int mgmt) {
    if (!sc.ready || sc.fatal || len < 24) return -1;
    txring_t *r = &sc.txq[DATA_QUEUE];
    if (r->queued >= IWN_TX_RING_HIMARK) { service(); if (r->queued >= IWN_TX_RING_HIMARK) return -1; }
    u8 type = frame[0] & 0x0c;
    int multicast = frame[4] & 1;
    int protect = (frame[1] & 0x40) && !multicast && sc.have_ptk && type == 0x08;
    u32 hdrlen = (type == 0x08 && (frame[0] & 0x80)) ? 26 : 24;
    usize body = len - hdrlen;
    u32 totlen = (u32)len + (protect ? 8 : 0);               /* the CCMP header; the MIC comes from the firmware */
    int idx = r->cur;
    u8 *slot = r->slots.va + (usize)idx * SLOT;
    u64 pa = r->slots.pa + (u64)idx * SLOT;
    u32 seg0 = 4 + sizeof(struct iwn_cmd_data) + hdrlen, pad = (hdrlen & 3) ? 4 - (hdrlen & 3) : 0;
    u32 payload_off = (seg0 + pad + 63) & ~63u;
    if (payload_off + body + 8 > SLOT) return -1;
    memset(slot, 0, payload_off);
    slot[0] = IWN_CMD_TX_DATA; slot[1] = 0; slot[2] = (u8)idx; slot[3] = (u8)r->qid;
    struct iwn_cmd_data *tx = (void *)(slot + 4);
    u32 flags = 0;
    if (!multicast) flags |= IWN_TX_NEED_ACK;
    if (pad) flags |= IWN_TX_NEED_PADDING;
    tx->id = (multicast || type != 0x08 || !sc.associated) ? IWN5000_ID_BROADCAST : IWN_ID_BSS;
    if (type == 0x00) tx->timeout = (frame[0] & 0xf0) == 0x00 || (frame[0] & 0xf0) == 0x20 ? 3 : 2;   /* (re)assoc request: 3 */
    tx->len = (u16)totlen;
    tx->tid = IWN_NONQOS_TID;
    tx->rts_ntries = 60;
    tx->data_ntries = 15;
    tx->lifetime = IWN_LIFETIME_INFINITE;
    u8 txant = (u8)IWN_LSB(sc.txchainmask);
    if (tx->id == IWN5000_ID_BROADCAST) {
        int ridx = sc.channel > 14 ? IWN_RIDX_OFDM6 : IWN_RIDX_CCK1;
        tx->plcp = iwn_rates[ridx].plcp;
        tx->rflags = (u8)(iwn_rates[ridx].flags | IWN_RFLAG_ANT(txant));
        tx->linkq = 0;
    } else {
        tx->plcp = iwn_rates[sc.channel > 14 ? IWN_RIDX_OFDM6 : IWN_RIDX_CCK1].plcp;
        tx->rflags = (u8)iwn_rates[sc.channel > 14 ? IWN_RIDX_OFDM6 : IWN_RIDX_CCK1].flags;
        tx->linkq = 0;
        flags |= IWN_TX_LINKQ;                               /* the link-quality table's rates and retries */
    }
    u64 scratch = pa + SCRATCH_OFF;                          /* data->scratch_paddr in iwn_alloc_tx_ring */
    (void)mgmt;
    tx->loaddr = IWN_LOADDR(scratch);
    tx->hiaddr = (u8)IWN_HIADDR(scratch);
    memcpy((u8 *)(tx + 1), frame, hdrlen);
    u8 *pl = slot + payload_off;
    u32 plen;
    if (protect) {
        sc.tsc++;
        pl[0] = (u8)sc.tsc; pl[1] = (u8)(sc.tsc >> 8); pl[2] = 0; pl[3] = 0x20;   /* ExtIV, key 0 */
        pl[4] = (u8)(sc.tsc >> 16); pl[5] = (u8)(sc.tsc >> 24); pl[6] = (u8)(sc.tsc >> 32); pl[7] = (u8)(sc.tsc >> 40);
        memcpy(pl + 8, frame + hdrlen, body);
        plen = (u32)body + 8;
        tx->security = IWN_CIPHER_CCMP;
        memcpy(tx->key, sc.ptk_tk, 16);
        totlen += 8;                                         /* the scheduler counts the MIC too */
    } else {
        memcpy(pl, frame + hdrlen, body);
        plen = (u32)body;
        tx->security = 0;
    }
    tx->flags = flags;
    struct iwn_tx_desc *desc = (struct iwn_tx_desc *)r->desc.va + idx;
    memset(desc, 0, sizeof *desc);
    desc->nsegs = plen ? 2 : 1;
    desc->segs[0].addr = IWN_LOADDR(pa);
    desc->segs[0].len = (u16)(IWN_HIADDR(pa) | (seg0 + pad) << 4);
    if (plen) {
        u64 ppa = pa + payload_off;
        desc->segs[1].addr = IWN_LOADDR(ppa);
        desc->segs[1].len = (u16)(IWN_HIADDR(ppa) | plen << 4);
    }
    update_sched(r->qid, idx, tx->id, (u16)totlen);
    r->used[idx] = 1;
    r->queued++;
    barrier();
    r->cur = (idx + 1) % IWN_TX_RING_COUNT;
    WR(IWN_HBUS_TARG_WRPTR, (u32)(r->qid << 8 | r->cur));
    sc.tx_frames++;
    return 0;
}

/* ---- the interface -------------------------------------------------------------------- */
int iwn_probe(pci_dev_t *d) {
    if (d->vendor != 0x8086) return 0;
    switch (d->device) {
    case 0x088e: case 0x088f:                                /* Centrino Advanced-N 6235 */
    case 0x0090: case 0x0091:                                /* Centrino Advanced-N 6230 */
        sc.pci = d; sc.present = 1; return 1;
    }
    return 0;
}

int iwn_present(void) { return sc.present; }
int iwn_ready(void) { return sc.ready && !sc.fatal; }
const u8 *iwn_macaddr(void) { return sc.macaddr; }
const char *iwn_fw_version(void) { return sc.fwver; }
void iwn_set_rx(iwm_rx_fn fn) { sc.rx = fn; }

void iwn_check_firmware(void) {
    static int done;
    if (done || sc.present) return;
    done = 1;
    if (read_firmware() == 0)
        LOG("firmware file checked: %s (init %u+%u, runtime %u+%u bytes), ready for an Intel 6230/6235",
            sc.fwver, sc.init.textsz, sc.init.datasz, sc.main.textsz, sc.main.datasz);
}

const char *iwn_status(void) {
    if (!sc.present) return "no supported Intel wireless card";
    if (sc.fatal) fmt(sc.status, sizeof sc.status, "Intel Centrino Advanced-N 6235: stopped after an error (see the log)");
    else if (!sc.attached) fmt(sc.status, sizeof sc.status, "Intel Centrino Advanced-N 6235: not started");
    else fmt(sc.status, sizeof sc.status, "Intel Centrino Advanced-N 6235 (%s), %s, %s; %llu frames in (%llu dropped), %llu out, %llu failed",
             sc.domain, sc.fwver, sc.ready ? "running" : "idle", sc.rx_frames, sc.rx_dropped, sc.tx_frames, sc.tx_fail);
    return sc.status;
}

int iwn_attach(void) {
    if (!sc.present) return -1;
    if (sc.attached) return 0;
    pci_dev_t *p = sc.pci;
    u32 c = pci_read32(p->bus, p->dev, p->fn, 0x04);
    pci_write32(p->bus, p->dev, p->fn, 0x04, (c & 0xffff) | 0x0006 | 0x0400);   /* memory, bus master; INTx off */
    u32 r40 = pci_read32(p->bus, p->dev, p->fn, 0x40);
    if (r40 & 0xff00) pci_write32(p->bus, p->dev, p->fn, 0x40, r40 & ~0xff00u);   /* PCI retry timeout */
    int pm = pci_find_cap(p->bus, p->dev, p->fn, 0x01);
    if (pm) {
        u32 pmcsr = pci_read32(p->bus, p->dev, p->fn, (u16)(pm + 4));
        if (pmcsr & 3) { pci_write32(p->bus, p->dev, p->fn, (u16)(pm + 4), pmcsr & ~3u); DELAY(10000); }
    }
    sc.cap_off = pci_find_cap(p->bus, p->dev, p->fn, 0x10);
    u64 bar = pci_bar(p->bus, p->dev, p->fn, 0);
    if (!bar) { LOG("BAR0 not assigned"); return -1; }
    sc.regs = (volatile u8 *)(usize)bar;
    u32 rev = RD(IWN_HW_REV);
    if (rev == 0xffffffffu) { LOG("the card does not answer (powered off?)"); return -1; }
    sc.hw_type = (int)((rev >> 4) & 0x1f);
    LOG("Intel %04x at %02x:%02x.%x, registers at 0x%llx, hardware type %d", p->device, p->bus, p->dev, p->fn, bar, sc.hw_type);
    if (sc.hw_type != IWN_HW_REV_TYPE_6005) { LOG("adapter type %d is not one this driver knows", sc.hw_type); return -1; }
    sc.flags |= F_ADV_BT_COEX;
    if (hw_prepare()) { LOG("hardware not ready"); return -1; }
    if (read_eeprom()) { LOG("could not read the EEPROM"); return -1; }
    if (read_firmware()) return -1;
    if (alloc_rings()) { LOG("out of DMA memory"); return -1; }
    WR(IWN_INT, 0xffffffff);
    LOG("MAC %02x:%02x:%02x:%02x:%02x:%02x, %dT%dR, regulatory domain %s, %d channels%s, firmware %s",
        sc.macaddr[0], sc.macaddr[1], sc.macaddr[2], sc.macaddr[3], sc.macaddr[4], sc.macaddr[5],
        sc.ntxchains, sc.nrxchains, sc.domain, sc.nchans, (sc.flags & F_5GHZ) ? " (2.4 and 5 GHz)" : "", sc.fwver);
    sc.attached = 1;
    return 0;
}

int iwn_start(void) {
    if (!sc.attached && iwn_attach()) return -1;
    if (sc.ready) return 0;
    sc.fatal = 0;
    if (hw_prepare()) { LOG("hardware not ready"); return -1; }
    if (!(RD(IWN_GP_CNTRL) & IWN_GP_CNTRL_RFKILL)) { LOG("radio switched off by the hardware switch"); return -1; }
    u64 t0 = k_now_ms();
    if (hw_init() || config()) { hw_stop(); return -1; }
    sc.ready = 1;
    LOG("ready in %llu ms", k_now_ms() - t0);
    return 0;
}

void iwn_stop(void) {
    if (!sc.attached) return;
    iwn_disconnect();
    hw_stop();
    sc.ready = 0;
    sc.scanning = 0;
}

void iwn_poll(void) {
    if (!sc.attached || !sc.regs) return;
    service();
    static int delivering;
    if (delivering) return;
    delivering = 1;
    for (int budget = 0; rxq_count && budget < RXQ_N; budget++) {
        int slot = rxq_head;
        if (sc.rx) sc.rx(rxq_buf[slot].frame, rxq_buf[slot].len, &rxq_buf[slot].ri);
        if (!rxq_count || rxq_head != slot) break;
        rxq_head = (rxq_head + 1) % RXQ_N;
        rxq_count--;
    }
    delivering = 0;
}
