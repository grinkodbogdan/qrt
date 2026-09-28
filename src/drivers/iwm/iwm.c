/*
 * iwm.c - Intel Wireless 8260 (7000/8000 family) for QRT.
 *
 * A port of the parts of OpenBSD's iwm(4) that a client station needs:
 * device bring-up, firmware loading (with the 8000 family's CPU1/CPU2
 * sections and firmware paging), NVM, calibration, UMAC scanning, the
 * MAC/PHY/binding/station contexts for association, the TX/RX rings, and
 * hardware CCMP for the pairwise key.  Register definitions and firmware
 * structures come unchanged from OpenBSD's if_iwmreg.h.
 *
 * Differences from iwm(4):
 *   - no interrupts: the driver polls CSR_INT and the RX status area.
 *     INTx is disabled in PCI config and MSI is never enabled, so the
 *     device's interrupt causes latch but nothing reaches the CPU; this
 *     works identically in firmware mode and native mode;
 *   - no ICT (interrupt cause table): CSR_INT is read directly;
 *   - legacy (non-HT) rates only, no aggregation, one station;
 *   - net80211's work (scan results, MLME, WPA) is in src/net/wlan.c.
 *
 * Original copyright:
 *
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 *   Author: Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017 Stefan Sperling <stsp@openbsd.org>
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
 *
 * Based on BSD-licensed source modules in the Linux iwlwifi driver (dual
 * BSD/GPLv2; Copyright(c) 2007 - 2014 Intel Corporation, 2013 - 2015 Intel
 * Mobile Communications GmbH, 2016 - 2017 Intel Deutschland GmbH).
 */
#include "iwm_compat.h"
#include "iwm.h"
#include "../../kernel/vfs.h"
#include "../../net/wifilog.h"

#define LOG(...) wifilog("iwm: " __VA_ARGS__)

#define FW_PATH       "/lib/firmware/iwlwifi-8000C-36.ucode"
#define SLOT          4096                     /* per-entry command / frame buffer */
#define N_TXQ         IWM_MAX_QUEUES
#define DATA_QUEUE    (IWM_DQA_MIN_MGMT_QUEUE + 0) /* EDCA best effort: all our frames */
#define AUX_QUEUE     IWM_DQA_AUX_QUEUE

enum { UC_REGULAR, UC_INIT, UC_WOW, UC_USNIFFER, UC_MAX };

typedef struct { u8 *va; u64 pa; usize size; } dma_t;

typedef struct {
    dma_t desc;                 /* 256 TFDs */
    dma_t slots;                /* 256 x SLOT: command header + TX command + payload */
    int qid, cur, tail, queued;
    u8 used[IWM_TX_RING_COUNT];
} txring_t;

typedef struct { u8 *data; u16 size; } phydb_entry_t;

static struct {
    int present, attached, ready, fatal;
    pci_dev_t *pci;
    volatile u8 *regs;
    u32 hw_rev;
    char status[128];

    /* firmware file */
    u8 *fw; usize fw_len;
    struct {
        struct { const u8 *data; u32 len, devoff; } sect[IWM_UCODE_SECT_MAX];
        int count;
        u32 paging_mem_size;
    } fws[UC_MAX];
    u32 capaflags, fw_phy_config;
    u8 enabled_capa[16], ucode_api[16];
    int n_scan_channels;
    struct { u32 flow, event; } calib[UC_MAX];
    char fwver[32];

    /* DMA */
    dma_t fw_dma, kw, sched;
    txring_t txq[N_TXQ];
    struct { dma_t desc, stat, bufs; int cur; } rxq;
    dma_t paging[IWM_NUM_OF_FW_PAGING_BLOCKS];
    int num_paging_blk, pages_in_last_blk;

    /* runtime */
    int nic_locks, cmdqid, uc_current;
    volatile int fw_chunk_done, uc_intr, uc_ok;
    volatile int init_complete;
    u32 sched_base, error_table, umac_error_table;
    u8 *resp[IWM_TX_RING_COUNT];
    u32 resp_len[IWM_TX_RING_COUNT];
    volatile u8 cmd_done[IWM_TX_RING_COUNT];
    phydb_entry_t phydb_cfg, phydb_nch, phydb_papd[IWM_NUM_PAPD_CH_GROUPS], phydb_txp[IWM_NUM_TXP_CH_GROUPS];

    /* NVM */
    u8 macaddr[6];
    int band_5ghz, lar_enabled;
    u8 valid_tx_ant, valid_rx_ant;
    u16 nvm_version;
    u8 channels[64]; int n_channels;          /* valid channel numbers */
    u8 passive[64];

    /* association */
    int mac_active, binding_active, sta_active, te_active;
    u32 te_uid;
    int scanning;
    u8 bssid[6];
    int phy_channel;
    u16 seq;
    u64 tsc;                                   /* CCMP packet number for our TX */
    int have_ptk;
    u8 ptk_tk[16];
    iwm_rx_fn rx;
    struct iwm_rx_phy_info last_phy;
    u64 rx_frames, tx_frames, tx_fail;
    u32 missed_beacons;
} sc;

/* ---- register access ------------------------------------------------------ */
static inline u32 RD(u32 reg) { return *(volatile u32 *)(sc.regs + reg); }
static inline void WR(u32 reg, u32 v) { *(volatile u32 *)(sc.regs + reg) = v; }
static inline void WR1(u32 reg, u8 v) { *(volatile u8 *)(sc.regs + reg) = v; }
static inline void SETB(u32 reg, u32 m) { WR(reg, RD(reg) | m); }
static inline void CLRB(u32 reg, u32 m) { WR(reg, RD(reg) & ~m); }
static inline void barrier(void) { __asm__ volatile("mfence" ::: "memory"); }
static void DELAY(u32 us) { hal_delay_us(us); }

static int isset_bit(const u8 *a, int bit) { return (a[bit >> 3] >> (bit & 7)) & 1; }
static void set_bit(u8 *a, int bit) { a[bit >> 3] |= (u8)(1 << (bit & 7)); }
static int capa(int bit) { return bit < 128 && isset_bit(sc.enabled_capa, bit); }
static int api(int bit) { return bit < 128 && isset_bit(sc.ucode_api, bit); }

static dma_t dma_new(usize size) {
    dma_t d = { 0 };
    d.va = hal_dma_alloc(size);
    d.pa = (u64)(usize)d.va;
    d.size = size;
    return d;
}

static u32 prph_read(u32 addr) {
    WR(IWM_HBUS_TARG_PRPH_RADDR, (addr & 0x000fffff) | (3u << 24));
    barrier();
    return RD(IWM_HBUS_TARG_PRPH_RDAT);
}
static void prph_write(u32 addr, u32 v) {
    WR(IWM_HBUS_TARG_PRPH_WADDR, (addr & 0x000fffff) | (3u << 24));
    barrier();
    WR(IWM_HBUS_TARG_PRPH_WDAT, v);
}

static int poll_bit(u32 reg, u32 bits, u32 mask, int timo_us) {
    for (;;) {
        if ((RD(reg) & mask) == (bits & mask)) return 1;
        if (timo_us < 10) return 0;
        timo_us -= 10;
        DELAY(10);
    }
}

static int nic_lock(void) {
    if (sc.nic_locks > 0) { sc.nic_locks++; return 1; }
    SETB(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    DELAY(2);
    if (poll_bit(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN,
                 IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY | IWM_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP, 150000)) {
        sc.nic_locks++;
        return 1;
    }
    LOG("acquiring the device failed (GP_CNTRL %08x)", RD(IWM_CSR_GP_CNTRL));
    return 0;
}
static void nic_unlock(void) {
    if (sc.nic_locks > 0 && --sc.nic_locks == 0)
        CLRB(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

static int set_bits_mask_prph(u32 reg, u32 bits, u32 mask) {
    if (!nic_lock()) return -1;
    prph_write(reg, (prph_read(reg) & mask) | bits);
    nic_unlock();
    return 0;
}
static int set_bits_prph(u32 reg, u32 bits) { return set_bits_mask_prph(reg, bits, ~0u); }
static int clear_bits_prph(u32 reg, u32 bits) { return set_bits_mask_prph(reg, 0, ~bits); }

static int read_mem(u32 addr, void *buf, int dwords) {
    u32 *v = buf;
    if (!nic_lock()) return -1;
    WR(IWM_HBUS_TARG_MEM_RADDR, addr);
    for (int i = 0; i < dwords; i++) v[i] = RD(IWM_HBUS_TARG_MEM_RDAT);
    nic_unlock();
    return 0;
}
static int write_mem(u32 addr, const void *buf, int dwords) {
    const u32 *v = buf;
    if (!nic_lock()) return -1;
    WR(IWM_HBUS_TARG_MEM_WADDR, addr);
    for (int i = 0; i < dwords; i++) WR(IWM_HBUS_TARG_MEM_WDAT, v ? v[i] : 0);
    nic_unlock();
    return 0;
}
static int write_mem32(u32 addr, u32 v) { return write_mem(addr, &v, 1); }

/* ---- firmware file (TLV format) ------------------------------------------------ */
static int store_section(int type, const u8 *data, u32 len) {
    if (type >= UC_MAX || len < 4 || sc.fws[type].count >= IWM_UCODE_SECT_MAX) return -1;
    int i = sc.fws[type].count++;
    memcpy(&sc.fws[type].sect[i].devoff, data, 4);
    sc.fws[type].sect[i].data = data + 4;
    sc.fws[type].sect[i].len = len - 4;
    return 0;
}

static int read_firmware(void) {
    vnode_t *n = vfs_lookup(FW_PATH);
    if (!n || !n->data || n->size < 88) { LOG("firmware %s not found on the boot stick", FW_PATH); return -1; }
    sc.fw = n->data;
    sc.fw_len = (usize)n->size;
    const struct iwm_tlv_ucode_header *h = (const void *)sc.fw;
    if (*(const u32 *)sc.fw != 0 || h->magic != IWM_TLV_UCODE_MAGIC) { LOG("invalid firmware file"); return -1; }
    fmt(sc.fwver, sizeof sc.fwver, "%u.%u.%u", IWM_UCODE_MAJOR(h->ver), IWM_UCODE_MINOR(h->ver), IWM_UCODE_API(h->ver));
    sc.n_scan_channels = 40;
    const u8 *p = h->data;
    usize len = sc.fw_len - sizeof(*h);
    while (len >= sizeof(struct iwm_ucode_tlv)) {
        const struct iwm_ucode_tlv *t = (const void *)p;
        u32 tl = t->length, ty = t->type;
        p += sizeof(*t); len -= sizeof(*t);
        if (len < tl) { LOG("firmware truncated"); return -1; }
        const u8 *d = p;
        switch (ty) {
        case IWM_UCODE_TLV_FLAGS: if (tl >= 4) sc.capaflags = *(const u32 *)d; break;
        case IWM_UCODE_TLV_SEC_RT: store_section(UC_REGULAR, d, tl); break;
        case IWM_UCODE_TLV_SEC_INIT: store_section(UC_INIT, d, tl); break;
        case IWM_UCODE_TLV_SEC_WOWLAN: store_section(UC_WOW, d, tl); break;
        case IWM_UCODE_TLV_SEC_RT_USNIFFER: store_section(UC_USNIFFER, d, tl); break;
        case IWM_UCODE_TLV_DEF_CALIB:
            if (tl == 12) {
                u32 type = *(const u32 *)d;
                if (type < UC_MAX) { sc.calib[type].flow = ((const u32 *)d)[1]; sc.calib[type].event = ((const u32 *)d)[2]; }
            }
            break;
        case IWM_UCODE_TLV_PHY_SKU: if (tl == 4) sc.fw_phy_config = *(const u32 *)d; break;
        case IWM_UCODE_TLV_API_CHANGES_SET:
        case IWM_UCODE_TLV_ENABLED_CAPABILITIES:
            if (tl == 8) {
                u32 idx = ((const u32 *)d)[0], bits = ((const u32 *)d)[1];
                u8 *arr = ty == IWM_UCODE_TLV_API_CHANGES_SET ? sc.ucode_api : sc.enabled_capa;
                if (idx < 4) for (int i = 0; i < 32; i++) if (bits & (1u << i)) set_bit(arr, (int)(i + 32 * idx));
            }
            break;
        case IWM_UCODE_TLV_PAGING:
            if (tl == 4) {
                u32 sz = *(const u32 *)d;
                if (sz > IWM_MAX_PAGING_IMAGE_SIZE || (sz & (IWM_FW_PAGING_SIZE - 1))) { LOG("bad paging size %u", sz); return -1; }
                sc.fws[UC_REGULAR].paging_mem_size = sz;
                sc.fws[UC_USNIFFER].paging_mem_size = sz;
            }
            break;
        case IWM_UCODE_TLV_N_SCAN_CHANNELS: if (tl == 4) sc.n_scan_channels = (int)*(const u32 *)d; break;
        case IWM_UCODE_TLV_FW_VERSION:
            if (tl == 12) fmt(sc.fwver, sizeof sc.fwver, "%u.%x.%u", ((const u32 *)d)[0], ((const u32 *)d)[1], ((const u32 *)d)[2]);
            break;
        default: break;              /* debug, command versions, ...: not needed */
        }
        usize adv = (tl + 3) & ~3u;
        if (adv > len) break;
        p += adv; len -= adv;
    }
    if (sc.n_scan_channels > 52) sc.n_scan_channels = 52;
    LOG("firmware %s: %d init + %d regular sections, paging %u bytes, %d scan channels",
        sc.fwver, sc.fws[UC_INIT].count, sc.fws[UC_REGULAR].count, sc.fws[UC_REGULAR].paging_mem_size, sc.n_scan_channels);
    LOG("capabilities: DQA %d, UMAC scan %d, LAR %d, adaptive dwell %d, STA type %d, TKIP/MIC keys %d",
        capa(IWM_UCODE_TLV_CAPA_DQA_SUPPORT), capa(IWM_UCODE_TLV_CAPA_UMAC_SCAN), capa(IWM_UCODE_TLV_CAPA_LAR_SUPPORT),
        api(IWM_UCODE_TLV_API_ADAPTIVE_DWELL), api(IWM_UCODE_TLV_API_STA_TYPE), api(IWM_UCODE_TLV_API_TKIP_MIC_KEYS));
    if (!capa(IWM_UCODE_TLV_CAPA_DQA_SUPPORT) || !capa(IWM_UCODE_TLV_CAPA_UMAC_SCAN) ||
        api(IWM_UCODE_TLV_API_ADAPTIVE_DWELL_V2) || api(IWM_UCODE_TLV_API_SCAN_EXT_CHAN_VER) ||
        capa(IWM_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS) || capa(IWM_UCODE_TLV_CAPA_BINDING_CDB_SUPPORT)) {
        LOG("this firmware uses command formats QRT's port does not implement");
        return -1;
    }
    return 0;
}

/* ---- rings ------------------------------------------------------------------------ */
static int alloc_rings(void) {
    sc.fw_dma = dma_new(IWM_FWDMASEGSZ_8000);
    sc.kw = dma_new(4096);
    sc.sched = dma_new(N_TXQ * sizeof(struct iwm_agn_scd_bc_tbl));
    for (int q = 0; q < N_TXQ; q++) {
        txring_t *r = &sc.txq[q];
        r->qid = q;
        r->desc = dma_new(IWM_TX_RING_COUNT * sizeof(struct iwm_tfd));
        if (!r->desc.va) return -1;
        /* frame/command buffers only for the queues QRT uses */
        if (q == IWM_DQA_CMD_QUEUE || q == AUX_QUEUE || q == DATA_QUEUE) {
            r->slots = dma_new((usize)IWM_TX_RING_COUNT * SLOT);
            if (!r->slots.va) return -1;
        }
    }
    sc.rxq.desc = dma_new(IWM_RX_RING_COUNT * sizeof(u32));
    sc.rxq.stat = dma_new(4096);
    sc.rxq.bufs = dma_new((usize)IWM_RX_RING_COUNT * IWM_RBUF_SIZE);
    if (!sc.fw_dma.va || !sc.kw.va || !sc.sched.va || !sc.rxq.desc.va || !sc.rxq.stat.va || !sc.rxq.bufs.va) return -1;
    u32 *d = (u32 *)sc.rxq.desc.va;
    for (int i = 0; i < IWM_RX_RING_COUNT; i++) d[i] = (u32)((sc.rxq.bufs.pa + (u64)i * IWM_RBUF_SIZE) >> 8);
    return 0;
}

static void reset_rings(void) {
    sc.rxq.cur = 0;
    memset(sc.rxq.stat.va, 0, sizeof(struct iwm_rb_status));
    for (int q = 0; q < N_TXQ; q++) {
        txring_t *r = &sc.txq[q];
        memset(r->desc.va, 0, r->desc.size);
        memset(r->used, 0, sizeof r->used);
        r->cur = r->tail = r->queued = 0;
    }
    for (int i = 0; i < IWM_TX_RING_COUNT; i++) {
        if (sc.resp[i]) { kfree(sc.resp[i]); sc.resp[i] = NULL; }
        sc.cmd_done[i] = 0;
    }
}

/* ---- power-up and reset (iwm_prepare_card_hw, iwm_apm_init, iwm_start_hw, ...) ---- */
static int set_hw_ready(void) {
    SETB(IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
    int ready = poll_bit(IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
                         IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY, 50);
    if (ready) SETB(IWM_CSR_MBOX_SET_REG, IWM_CSR_MBOX_SET_REG_OS_ALIVE);
    return ready;
}

static int prepare_card_hw(void) {
    if (set_hw_ready()) return 0;
    SETB(IWM_CSR_DBG_LINK_PWR_MGMT_REG, IWM_CSR_RESET_LINK_PWR_MGMT_DISABLED);
    DELAY(1000);
    int t = 0;
    for (int tries = 0; tries < 10; tries++) {
        SETB(IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_PREPARE);
        do {
            if (set_hw_ready()) return 0;
            DELAY(200);
            t += 200;
        } while (t < 150000);
        DELAY(25000);
    }
    LOG("card not ready (HW_IF_CONFIG %08x)", RD(IWM_CSR_HW_IF_CONFIG_REG));
    return -1;
}

static void apm_config(void) {
    /* disable L0s when the BIOS enabled ASPM L1 (PCIe link control, cap + 0x10) */
    int cap = pci_find_cap(sc.pci->bus, sc.pci->dev, sc.pci->fn, 0x10);
    u32 lctl = cap ? pci_read32(sc.pci->bus, sc.pci->dev, sc.pci->fn, (u16)(cap + 0x10)) : 0;
    if (lctl & 0x2) SETB(IWM_CSR_GIO_REG, IWM_CSR_GIO_REG_VAL_L0S_ENABLED);
    else CLRB(IWM_CSR_GIO_REG, IWM_CSR_GIO_REG_VAL_L0S_ENABLED);
}

static int apm_init(void) {
    SETB(IWM_CSR_GIO_CHICKEN_BITS, IWM_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
    SETB(IWM_CSR_DBG_HPET_MEM_REG, IWM_CSR_DBG_HPET_MEM_REG_VAL);
    SETB(IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);
    apm_config();
    SETB(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    if (!poll_bit(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                  IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000)) {
        LOG("timeout waiting for the MAC clock");
        return -1;
    }
    return 0;
}

static void apm_stop(void) {
    SETB(IWM_CSR_DBG_LINK_PWR_MGMT_REG, IWM_CSR_RESET_LINK_PWR_MGMT_DISABLED);
    SETB(IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_PREPARE | IWM_CSR_HW_IF_CONFIG_REG_ENABLE_PME);
    DELAY(1000);
    CLRB(IWM_CSR_DBG_LINK_PWR_MGMT_REG, IWM_CSR_RESET_LINK_PWR_MGMT_DISABLED);
    DELAY(5000);
    SETB(IWM_CSR_RESET, IWM_CSR_RESET_REG_FLAG_STOP_MASTER);
    if (!poll_bit(IWM_CSR_RESET, IWM_CSR_RESET_REG_FLAG_MASTER_DISABLED, IWM_CSR_RESET_REG_FLAG_MASTER_DISABLED, 100))
        LOG("timeout waiting for bus master to stop");
    CLRB(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
}

/* The device's own interrupt masks are programmed as iwm(4) does; nothing
 * reaches the CPU because INTx is disabled and MSI is off. */
static u32 intmask;
static void enable_interrupts(void) { intmask = IWM_CSR_INI_SET_MASK; WR(IWM_CSR_INT_MASK, intmask); }
static void enable_fwload_interrupt(void) { intmask = IWM_CSR_INT_BIT_FH_TX; WR(IWM_CSR_INT_MASK, intmask); }
static void enable_rfkill_int(void) { intmask = IWM_CSR_INT_BIT_RF_KILL; WR(IWM_CSR_INT_MASK, intmask); }
static void disable_interrupts(void) {
    WR(IWM_CSR_INT_MASK, 0);
    WR(IWM_CSR_INT, ~0u);
    WR(IWM_CSR_FH_INT_STATUS, ~0u);
}

static int rfkill(void) { return (RD(IWM_CSR_GP_CNTRL) & IWM_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) == 0; }

static int start_hw(void) {
    if (prepare_card_hw()) return -1;
    WR(IWM_CSR_RESET, IWM_CSR_RESET_REG_FLAG_SW_RESET);
    DELAY(5000);
    if (apm_init()) return -1;
    enable_rfkill_int();
    return 0;
}

static void disable_rx_dma(void) {
    if (!nic_lock()) return;
    WR(IWM_FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);
    for (int i = 0; i < 1000; i++) {
        if (RD(IWM_FH_MEM_RSSR_RX_STATUS_REG) & IWM_FH_RSSR_CHNL0_RX_STATUS_CHNL_IDLE) break;
        DELAY(10);
    }
    nic_unlock();
}

static void stop_device(void) {
    disable_interrupts();
    if (nic_lock()) {
        prph_write(IWM_SCD_TXFACT, 0);
        for (int ch = 0; ch < IWM_FH_TCSR_CHNL_NUM; ch++) {
            WR(IWM_FH_TCSR_CHNL_TX_CONFIG_REG(ch), 0);
            for (int i = 0; i < 200; i++) {
                if (RD(IWM_FH_TSSR_TX_STATUS_REG) & IWM_FH_TSSR_TX_STATUS_REG_MSK_CHNL_IDLE(ch)) break;
                DELAY(20);
            }
        }
        nic_unlock();
    }
    disable_rx_dma();
    reset_rings();
    CLRB(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    sc.nic_locks = 0;
    apm_stop();
    WR(IWM_CSR_RESET, IWM_CSR_RESET_REG_FLAG_SW_RESET);
    DELAY(5000);
    disable_interrupts();
    enable_rfkill_int();
    prepare_card_hw();
    sc.mac_active = sc.binding_active = sc.sta_active = sc.te_active = 0;
}

static void nic_config(void) {
    u32 type = (sc.fw_phy_config & IWM_FW_PHY_CFG_RADIO_TYPE) >> IWM_FW_PHY_CFG_RADIO_TYPE_POS;
    u32 step = (sc.fw_phy_config & IWM_FW_PHY_CFG_RADIO_STEP) >> IWM_FW_PHY_CFG_RADIO_STEP_POS;
    u32 dash = (sc.fw_phy_config & IWM_FW_PHY_CFG_RADIO_DASH) >> IWM_FW_PHY_CFG_RADIO_DASH_POS;
    u32 v = 0;
    v |= IWM_CSR_HW_REV_STEP(sc.hw_rev) << IWM_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP;
    v |= IWM_CSR_HW_REV_DASH(sc.hw_rev) << IWM_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH;
    v |= type << IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE;
    v |= step << IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP;
    v |= dash << IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH;
    u32 mask = IWM_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH | IWM_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP |
               IWM_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP | IWM_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH |
               IWM_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE | IWM_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI |
               IWM_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
    WR(IWM_CSR_HW_IF_CONFIG_REG, (RD(IWM_CSR_HW_IF_CONFIG_REG) & ~mask) | v);
}

static int nic_rx_init(void) {
    memset(sc.rxq.stat.va, 0, sizeof(struct iwm_rb_status));
    disable_rx_dma();
    if (!nic_lock()) return -1;
    WR(IWM_FH_MEM_RCSR_CHNL0_RBDCB_WPTR, 0);
    WR(IWM_FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ, 0);
    WR(IWM_FH_RSCSR_CHNL0_RDPTR, 0);
    WR(IWM_FH_RSCSR_CHNL0_RBDCB_WPTR_REG, 0);
    WR(IWM_FH_RSCSR_CHNL0_RBDCB_BASE_REG, (u32)(sc.rxq.desc.pa >> 8));
    WR(IWM_FH_RSCSR_CHNL0_STTS_WPTR_REG, (u32)(sc.rxq.stat.pa >> 4));
    WR(IWM_FH_MEM_RCSR_CHNL0_CONFIG_REG,
       IWM_FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL | IWM_FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY |
       IWM_FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL |
       (IWM_RX_RB_TIMEOUT << IWM_FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS) |
       IWM_FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K |
       (IWM_RX_QUEUE_SIZE_LOG << IWM_FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS));
    WR1(IWM_CSR_INT_COALESCING, IWM_HOST_INT_TIMEOUT_DEF);
    nic_unlock();
    WR(IWM_FH_RSCSR_CHNL0_WPTR, 8);
    sc.rxq.cur = 0;
    return 0;
}

static int nic_tx_init(void) {
    if (!nic_lock()) return -1;
    prph_write(IWM_SCD_TXFACT, 0);
    WR(IWM_FH_KW_MEM_ADDR_REG, (u32)(sc.kw.pa >> 4));
    for (int q = 0; q < N_TXQ; q++) WR(IWM_FH_MEM_CBBC_QUEUE(q), (u32)(sc.txq[q].desc.pa >> 8));
    int err = set_bits_prph(IWM_SCD_GP_CTRL, IWM_SCD_GP_CTRL_AUTO_ACTIVE_MODE | IWM_SCD_GP_CTRL_ENABLE_31_QUEUES);
    nic_unlock();
    return err;
}

static int nic_init(void) {
    if (apm_init()) return -1;
    nic_config();
    if (nic_rx_init() || nic_tx_init()) return -1;
    SETB(IWM_CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);
    return 0;
}

/* ---- scheduler byte-count table ---------------------------------------------------- */
static void update_sched(int qid, int idx, u8 sta_id, u16 len) {
    struct iwm_agn_scd_bc_tbl *t = (void *)sc.sched.va;
    len += IWM_TX_CRC_SIZE + IWM_TX_DELIMITER_SIZE;
    if (sc.capaflags & IWM_UCODE_TLV_FLAGS_DW_BC_TABLE) len = (u16)((len + 3) / 4);
    u16 v = (u16)(sta_id << 12 | len);
    t[qid].tfd_offset[idx] = v;
    if (idx < IWM_TFD_QUEUE_SIZE_BC_DUP) t[qid].tfd_offset[IWM_TFD_QUEUE_SIZE_MAX + idx] = v;
}

static void reset_sched(int qid, int idx, u8 sta_id) {
    struct iwm_agn_scd_bc_tbl *t = (void *)sc.sched.va;
    u16 v = (u16)(1 | (sta_id << 12));
    t[qid].tfd_offset[idx] = v;
    if (idx < IWM_TFD_QUEUE_SIZE_BC_DUP) t[qid].tfd_offset[IWM_TFD_QUEUE_SIZE_MAX + idx] = v;
}

static void set_tb(struct iwm_tfd_tb *tb, u64 pa, u32 len) {
    u32 lo = (u32)pa;
    memcpy(&tb->lo, &lo, 4);
    tb->hi_n_len = (u16)(iwm_get_dma_hi_addr(pa) | (len << 4));
}

/* ---- host commands ------------------------------------------------------------------ */
#define CMD_ASYNC     1
#define CMD_WANT_RESP 2

typedef struct {
    u32 id, flags;
    const void *data[2];
    u16 len[2];
    struct iwm_rx_packet *resp;              /* CMD_WANT_RESP: kfree when done */
    u32 resp_len;
} hcmd_t;

static void service(void);

static int send_cmd(hcmd_t *h) {
    txring_t *r = &sc.txq[sc.cmdqid];
    if (sc.fatal) return -1;
    if (r->queued >= IWM_TX_RING_COUNT - 4) { LOG("command queue full"); return -1; }
    int idx = r->cur;
    u32 paylen = (u32)h->len[0] + h->len[1];
    int group = iwm_cmd_groupid(h->id);
    u32 hdrlen = group ? sizeof(struct iwm_cmd_header_wide) : sizeof(struct iwm_cmd_header);
    if (hdrlen + paylen > SLOT) { LOG("command 0x%x too long (%u bytes)", h->id, paylen); return -1; }

    u8 *slot = r->slots.va + (usize)idx * SLOT;
    u64 pa = r->slots.pa + (u64)idx * SLOT;
    memset(slot, 0, hdrlen);
    if (group) {
        struct iwm_cmd_header_wide *w = (void *)slot;
        w->opcode = iwm_cmd_opcode(h->id);
        w->group_id = (u8)group;
        w->qid = (u8)r->qid;
        w->idx = (u8)idx;
        w->length = (u16)paylen;
        w->version = iwm_cmd_version(h->id);
    } else {
        struct iwm_cmd_header *c = (void *)slot;
        c->code = (u8)h->id;
        c->flags = 0;
        c->qid = (u8)r->qid;
        c->idx = (u8)idx;
    }
    u32 off = hdrlen;
    for (int i = 0; i < 2; i++) if (h->len[i]) { memcpy(slot + off, h->data[i], h->len[i]); off += h->len[i]; }

    struct iwm_tfd *desc = (struct iwm_tfd *)r->desc.va + idx;
    memset(desc, 0, sizeof *desc);
    set_tb(&desc->tbs[0], pa, hdrlen + paylen);
    desc->num_tbs = 1;

    if (sc.resp[idx]) { kfree(sc.resp[idx]); sc.resp[idx] = NULL; }
    if (h->flags & CMD_WANT_RESP) { sc.resp[idx] = kalloc(h->resp_len); sc.resp_len[idx] = h->resp_len; }
    sc.cmd_done[idx] = 0;

    update_sched(r->qid, idx, 0, 0);
    barrier();
    r->queued++;
    r->cur = (r->cur + 1) % IWM_TX_RING_COUNT;
    WR(IWM_HBUS_TARG_WRPTR, (u32)(r->qid << 8 | r->cur));
    if (h->flags & CMD_ASYNC) return 0;

    u64 deadline = k_now_ms() + 2000;
    while (!sc.cmd_done[idx] && !sc.fatal && k_now_ms() < deadline) { service(); if (!sc.cmd_done[idx]) DELAY(20); }
    if (!sc.cmd_done[idx]) {
        LOG("command 0x%x timed out%s", h->id, sc.fatal ? " (firmware error)" : "");
        return -1;
    }
    if (h->flags & CMD_WANT_RESP) {
        h->resp = (struct iwm_rx_packet *)sc.resp[idx];
        sc.resp[idx] = NULL;
        if (!h->resp || h->resp->len_n_flags == 0) { if (h->resp) kfree(h->resp); h->resp = NULL; return -1; }
    }
    return 0;
}

static int send_cmd_pdu(u32 id, u32 flags, u16 len, const void *data) {
    hcmd_t h = { .id = id, .flags = flags, .data = { data }, .len = { len } };
    return send_cmd(&h);
}

/* commands answered by a struct iwm_cmd_response status word */
static int send_cmd_status(u32 id, u16 len, const void *data, u32 *status) {
    hcmd_t h = { .id = id, .flags = CMD_WANT_RESP, .data = { data }, .len = { len },
                 .resp_len = sizeof(struct iwm_rx_packet) + sizeof(struct iwm_cmd_response) };
    if (send_cmd(&h)) return -1;
    int err = 0;
    if (h.resp->hdr.flags & IWM_CMD_FAILED_MSK) err = -1;
    else *status = ((struct iwm_cmd_response *)h.resp->data)->status;
    kfree(h.resp);
    return err;
}

/* ---- firmware error log (iwm_nic_error) -------------------------------------------- */
struct error_event_table {
    u32 valid, error_id, trm_hw_status0, trm_hw_status1, blink2, ilink1, ilink2, data1, data2, data3,
        bcon_time, tsf_low, tsf_hi, gp1, gp2, fw_rev_type, major, minor, hw_ver, brd_ver, log_pc,
        frame_ptr, stack_ptr, hcmd, isr0, isr1, isr2, isr3, isr4, last_cmd_id, wait_event,
        l2p_control, l2p_duration, l2p_mhvalid, l2p_addr_match, lmpm_pmg_sel, u_timestamp, flow_handler;
};

static void nic_error(void) {
    struct error_event_table t;
    u32 base = sc.error_table;
    if (base < 0x800000) { LOG("firmware error: no valid error log pointer (0x%08x)", base); return; }
    if (read_mem(base, &t, sizeof t / 4) || !t.valid) { LOG("firmware error: log unreadable"); return; }
    LOG("firmware error 0x%08x: pc/blink2 %08x ilink1 %08x ilink2 %08x data %08x %08x %08x",
        t.error_id, t.blink2, t.ilink1, t.ilink2, t.data1, t.data2, t.data3);
    LOG("  last host command 0x%08x (id %08x), hw %08x, fw %u.%u, isr %08x %08x %08x",
        t.hcmd, t.last_cmd_id, t.hw_ver, t.major, t.minor, t.isr0, t.isr1, t.isr2);
}

/* ---- received packets (iwm_rx_pkt) ---------------------------------------------------- */
static void phy_db_set(const struct iwm_calib_res_notif_phy_db *n) {
    u16 type = n->type, size = n->length;
    phydb_entry_t *e = NULL;
    u16 chg = 0;
    if (type == IWM_PHY_DB_CALIB_CHG_PAPD || type == IWM_PHY_DB_CALIB_CHG_TXP) chg = *(const u16 *)n->data;
    switch (type) {
    case IWM_PHY_DB_CFG: e = &sc.phydb_cfg; break;
    case IWM_PHY_DB_CALIB_NCH: e = &sc.phydb_nch; break;
    case IWM_PHY_DB_CALIB_CHG_PAPD: if (chg < IWM_NUM_PAPD_CH_GROUPS) e = &sc.phydb_papd[chg]; break;
    case IWM_PHY_DB_CALIB_CHG_TXP: if (chg < IWM_NUM_TXP_CH_GROUPS) e = &sc.phydb_txp[chg]; break;
    }
    if (!e) return;
    if (e->data) kfree(e->data);
    e->data = kalloc(size ? size : 1);
    memcpy(e->data, n->data, size);
    e->size = size;
}

static void tx_done(const struct iwm_rx_packet *pkt) {
    int qid = pkt->hdr.qid, idx = pkt->hdr.idx;
    if (qid >= N_TXQ) return;
    const struct iwm_tx_resp *tr = (const void *)pkt->data;
    txring_t *r = &sc.txq[qid];
    int status = tr->status.status & IWM_TX_STATUS_MSK;
    if (status != IWM_TX_STATUS_SUCCESS && status != IWM_TX_STATUS_DIRECT_DONE) {
        sc.tx_fail++;
        LOG("tx on queue %d slot %d failed: status 0x%x after %d retries", qid, idx, status, tr->failure_frame);
    }
    u32 ssn;
    memcpy(&ssn, &tr->status + tr->frame_count, 4);
    int until = (int)(ssn & 0xfff) & (IWM_TX_RING_COUNT - 1);
    while (r->tail != until) {
        if (r->used[r->tail]) { reset_sched(qid, r->tail, IWM_STATION_ID); r->used[r->tail] = 0; r->queued--; }
        r->tail = (r->tail + 1) % IWM_TX_RING_COUNT;
    }
}

static void rx_mpdu(const struct iwm_rx_packet *pkt, u32 maxlen) {
    const struct iwm_rx_mpdu_res_start *res = (const void *)pkt->data;
    u32 len = res->byte_count;
    if (len < sizeof(struct ieee80211_frame) || len + sizeof(*res) + 4 > maxlen) return;
    const u8 *frame = pkt->data + sizeof(*res);
    u32 st = *(const u32 *)(frame + len);
    if (!(st & IWM_RX_MPDU_RES_STATUS_CRC_OK) || !(st & IWM_RX_MPDU_RES_STATUS_OVERRUN_OK)) return;
    iwm_rxinfo_t ri = { 0 };
    ri.channel = (int)sc.last_phy.channel;
    ri.rstamp = sc.last_phy.system_timestamp;
    u32 e = sc.last_phy.non_cfg_phy[IWM_RX_INFO_ENERGY_ANT_ABC_IDX];
    int a = (int)((e & IWM_RX_INFO_ENERGY_ANT_A_MSK) >> IWM_RX_INFO_ENERGY_ANT_A_POS);
    int b = (int)((e & IWM_RX_INFO_ENERGY_ANT_B_MSK) >> IWM_RX_INFO_ENERGY_ANT_B_POS);
    a = a ? -a : -256; b = b ? -b : -256;
    ri.rssi = MAX(a, b);
    /* pairwise CCMP decrypted by the firmware (iwm_rx_hwdecrypt) */
    const struct ieee80211_frame *wh = (const void *)frame;
    if ((wh->i_fc[1] & 0x40) && !(wh->i_addr1[0] & 1) && sc.have_ptk) {
        if ((st & IWM_RX_MPDU_RES_STATUS_SEC_ENC_MSK) != IWM_RX_MPDU_RES_STATUS_SEC_CCM_ENC ||
            (st & (IWM_RX_MPDU_RES_STATUS_DEC_DONE | IWM_RX_MPDU_RES_STATUS_MIC_OK)) !=
                  (IWM_RX_MPDU_RES_STATUS_DEC_DONE | IWM_RX_MPDU_RES_STATUS_MIC_OK))
            return;                                 /* decryption failed: drop */
        ri.decrypted = 1;
    }
    sc.rx_frames++;
    if (sc.rx) sc.rx(frame, len, &ri);
}

static void rx_pkt(u8 *buf) {
    const u32 minsz = sizeof(u32) + sizeof(struct iwm_cmd_header);
    u32 off = 0;
    while (off + minsz < IWM_RBUF_SIZE) {
        struct iwm_rx_packet *pkt = (void *)(buf + off);
        int qid = pkt->hdr.qid, idx = pkt->hdr.idx;
        u32 code = IWM_WIDE_ID(pkt->hdr.flags, pkt->hdr.code);
        if ((qid & ~0x80) == 0 && idx == 0 && code == 0) break;
        if (pkt->len_n_flags == IWM_FH_RSCSR_FRAME_INVALID) break;
        u32 len = sizeof(u32) + (pkt->len_n_flags & IWM_FH_RSCSR_FRAME_SIZE_MSK);
        if (len < minsz || len > IWM_RBUF_SIZE - off) break;
        u32 paylen = len - sizeof(u32) - sizeof(struct iwm_cmd_header);

        switch (code) {
        case IWM_REPLY_RX_PHY_CMD: memcpy(&sc.last_phy, pkt->data, sizeof sc.last_phy); break;
        case IWM_REPLY_RX_MPDU_CMD: rx_mpdu(pkt, IWM_RBUF_SIZE - off - minsz); break;
        case IWM_TX_CMD: tx_done(pkt); break;
        case IWM_ALIVE: {
            u32 base = 0, umac = 0, sched = 0; int ok = 0;
            if (paylen == sizeof(struct iwm_alive_resp_v1)) {
                struct iwm_alive_resp_v1 *a = (void *)pkt->data;
                base = a->error_event_table_ptr; sched = a->scd_base_ptr; ok = a->status == IWM_ALIVE_STATUS_OK;
            } else if (paylen == sizeof(struct iwm_alive_resp_v2)) {
                struct iwm_alive_resp_v2 *a = (void *)pkt->data;
                base = a->error_event_table_ptr; sched = a->scd_base_ptr; umac = a->error_info_addr; ok = a->status == IWM_ALIVE_STATUS_OK;
            } else if (paylen == sizeof(struct iwm_alive_resp_v3)) {
                struct iwm_alive_resp_v3 *a = (void *)pkt->data;
                base = a->error_event_table_ptr; sched = a->scd_base_ptr; umac = a->error_info_addr; ok = a->status == IWM_ALIVE_STATUS_OK;
            } else LOG("alive notification of unknown size %u", paylen);
            sc.error_table = base; sc.umac_error_table = umac; sc.sched_base = sched;
            sc.uc_ok = ok;
            sc.uc_intr = 1;
            break;
        }
        case IWM_CALIB_RES_NOTIF_PHY_DB:
            phy_db_set((const void *)pkt->data);
            sc.init_complete |= IWM_CALIB_COMPLETE;
            break;
        case IWM_INIT_COMPLETE_NOTIF: sc.init_complete |= IWM_INIT_COMPLETE; break;
        case IWM_SCAN_COMPLETE_UMAC:
        case IWM_SCAN_ITERATION_COMPLETE_UMAC:
            if (sc.scanning) LOG("scan complete");
            sc.scanning = 0;
            break;
        case IWM_REPLY_ERROR: {
            struct iwm_error_resp *e = (void *)pkt->data;
            LOG("firmware reports error 0x%x for command 0x%x", e->error_type, e->cmd_id);
            break;
        }
        case IWM_TIME_EVENT_NOTIFICATION: {
            struct iwm_time_event_notif *n = (void *)pkt->data;
            if (n->unique_id == sc.te_uid && (n->action & IWM_TE_V2_NOTIF_HOST_EVENT_END)) sc.te_active = 0;
            break;
        }
        case IWM_MISSED_BEACONS_NOTIFICATION: {
            struct iwm_missed_beacons_notif *m = (void *)pkt->data;
            sc.missed_beacons = m->consec_missed_beacons;
            LOG("missed %u consecutive beacons", m->consec_missed_beacons);
            break;
        }
        case IWM_WIDE_ID(IWM_PHY_OPS_GROUP, IWM_CT_KILL_NOTIFICATION):
            LOG("device at critical temperature: stopping");
            sc.fatal = 1;
            break;
        default: break;          /* command responses and notifications QRT ignores */
        }

        /* a direct response to one of our commands (bit 7 marks firmware-originated ones) */
        if (!(qid & 0x80) && qid == sc.cmdqid && code != IWM_REPLY_RX_MPDU_CMD && code != IWM_REPLY_RX_PHY_CMD) {
            if (sc.resp[idx]) {
                u32 n = MIN(len, sc.resp_len[idx]);
                memcpy(sc.resp[idx], pkt, n);
            }
            if (!sc.cmd_done[idx]) {
                sc.cmd_done[idx] = 1;
                if (sc.txq[sc.cmdqid].queued > 0) sc.txq[sc.cmdqid].queued--;
            }
        }
        off += (len + IWM_FH_RSCSR_FRAME_ALIGN - 1) & ~(u32)(IWM_FH_RSCSR_FRAME_ALIGN - 1);
    }
}

static void rx_notif(void) {
    struct iwm_rb_status *st = (void *)sc.rxq.stat.va;
    barrier();
    int hw = st->closed_rb_num & 0xfff & (IWM_RX_RING_COUNT - 1);
    if (hw == sc.rxq.cur) return;
    while (sc.rxq.cur != hw) {
        rx_pkt(sc.rxq.bufs.va + (usize)sc.rxq.cur * IWM_RBUF_SIZE);
        sc.rxq.cur = (sc.rxq.cur + 1) % IWM_RX_RING_COUNT;
    }
    int w = hw == 0 ? IWM_RX_RING_COUNT - 1 : hw - 1;
    WR(IWM_FH_RSCSR_CHNL0_WPTR, (u32)(w & ~7));
}

/* The interrupt handler (iwm_intr), run by polling. */
static void service(void) {
    if (!sc.regs) return;
    u32 r1 = RD(IWM_CSR_INT);
    if (r1 == 0xffffffffu || (r1 & 0xfffffff0u) == 0xa5a5a5a0u) {
        if (!sc.fatal) LOG("device stopped answering (CSR_INT %08x)", r1);
        sc.fatal = 1;
        return;
    }
    if (r1) {
        WR(IWM_CSR_INT, r1 | ~intmask);
        if (r1 & IWM_CSR_INT_BIT_SW_ERR) {
            if (!sc.fatal) { LOG("fatal firmware error"); nic_error(); }
            sc.fatal = 1;
            return;
        }
        if (r1 & IWM_CSR_INT_BIT_HW_ERR) {
            if (!sc.fatal) LOG("hardware error");
            sc.fatal = 1;
            return;
        }
        if (r1 & IWM_CSR_INT_BIT_RF_KILL) LOG("radio switch: %s", rfkill() ? "off (rfkill)" : "on");
        if (r1 & IWM_CSR_INT_BIT_FH_TX) { WR(IWM_CSR_FH_INT_STATUS, IWM_CSR_FH_INT_TX_MASK); sc.fw_chunk_done = 1; }
        if (r1 & (IWM_CSR_INT_BIT_FH_RX | IWM_CSR_INT_BIT_SW_RX)) WR(IWM_CSR_FH_INT_STATUS, IWM_CSR_FH_INT_RX_MASK);
        if (r1 & IWM_CSR_INT_BIT_RX_PERIODIC) WR(IWM_CSR_INT, IWM_CSR_INT_BIT_RX_PERIODIC);
    }
    rx_notif();
}

static int wait_for(volatile int *flag, int want, u32 ms) {
    u64 deadline = k_now_ms() + ms;
    while (k_now_ms() < deadline && !sc.fatal) {
        service();
        if ((*flag & want) == want) return 0;
        DELAY(50);
    }
    return (*flag & want) == want ? 0 : -1;
}

/* ---- firmware loading (iwm_firmware_load_chunk, iwm_load_firmware_8000, ...) ---- */
static int load_chunk(u32 dst, const u8 *data, u32 len) {
    memcpy(sc.fw_dma.va, data, len);
    barrier();
    int ext = dst >= IWM_FW_MEM_EXTENDED_START && dst <= IWM_FW_MEM_EXTENDED_END;
    if (ext && set_bits_prph(IWM_LMPM_CHICK, IWM_LMPM_CHICK_EXTENDED_ADDR_SPACE)) return -1;
    sc.fw_chunk_done = 0;
    if (!nic_lock()) return -1;
    WR(IWM_FH_TCSR_CHNL_TX_CONFIG_REG(IWM_FH_SRVC_CHNL), IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);
    WR(IWM_FH_SRVC_CHNL_SRAM_ADDR_REG(IWM_FH_SRVC_CHNL), dst);
    WR(IWM_FH_TFDIB_CTRL0_REG(IWM_FH_SRVC_CHNL), (u32)(sc.fw_dma.pa & IWM_FH_MEM_TFDIB_DRAM_ADDR_LSB_MSK));
    WR(IWM_FH_TFDIB_CTRL1_REG(IWM_FH_SRVC_CHNL),
       ((u32)iwm_get_dma_hi_addr(sc.fw_dma.pa) << IWM_FH_MEM_TFDIB_REG1_ADDR_BITSHIFT) | len);
    WR(IWM_FH_TCSR_CHNL_TX_BUF_STS_REG(IWM_FH_SRVC_CHNL),
       1 << IWM_FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM | 1 << IWM_FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX |
       IWM_FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID);
    WR(IWM_FH_TCSR_CHNL_TX_CONFIG_REG(IWM_FH_SRVC_CHNL),
       IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE | IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
       IWM_FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);
    nic_unlock();
    int err = wait_for(&sc.fw_chunk_done, 1, 1000);
    if (err) LOG("firmware chunk at 0x%x (%u bytes) did not load (CSR_INT %08x)", dst, len, RD(IWM_CSR_INT));
    if (ext && clear_bits_prph(IWM_LMPM_CHICK, IWM_LMPM_CHICK_EXTENDED_ADDR_SPACE) && !err) err = -1;
    return err;
}

static int load_sect(u32 dst, const u8 *data, u32 len) {
    for (u32 off = 0; off < len; off += IWM_FH_MEM_TB_MAX_LENGTH) {
        u32 n = MIN((u32)IWM_FH_MEM_TB_MAX_LENGTH, len - off);
        if (load_chunk(dst + off, data + off, n)) return -1;
    }
    return 0;
}

static int load_cpu_sections_8000(int type, int cpu, int *first) {
    int shift = cpu == 1 ? 0 : 16, i, last = 0;
    u32 sec_num = 1;
    if (cpu == 1) *first = 0; else (*first)++;
    for (i = *first; i < IWM_UCODE_SECT_MAX; i++) {
        last = i;
        const u8 *data = sc.fws[type].sect[i].data;
        u32 len = sc.fws[type].sect[i].len, off = sc.fws[type].sect[i].devoff;
        if (!data || off == IWM_CPU1_CPU2_SEPARATOR_SECTION || off == IWM_PAGING_SEPARATOR_SECTION) break;
        if (len > IWM_FWDMASEGSZ_8000 || load_sect(off, data, len)) { LOG("could not load firmware section %d", i); return -1; }
        if (!nic_lock()) return -1;
        WR(IWM_FH_UCODE_LOAD_STATUS, RD(IWM_FH_UCODE_LOAD_STATUS) | (sec_num << shift));
        sec_num = (sec_num << 1) | 1;
        nic_unlock();
    }
    *first = last;
    if (!nic_lock()) return -1;
    WR(IWM_FH_UCODE_LOAD_STATUS, cpu == 1 ? 0xFFFF : 0xFFFFFFFF);
    nic_unlock();
    return 0;
}

static int load_firmware(int type) {
    sc.uc_intr = sc.uc_ok = 0;
    if (nic_lock()) { prph_write(IWM_RELEASE_CPU_RESET, IWM_RELEASE_CPU_RESET_BIT); nic_unlock(); }
    int first;
    if (load_cpu_sections_8000(type, 1, &first) || load_cpu_sections_8000(type, 2, &first)) return -1;
    enable_interrupts();
    if (wait_for(&sc.uc_intr, 1, 1000) || !sc.uc_ok) {
        LOG("firmware did not report alive (%s)", sc.uc_intr ? "bad status" : "timeout");
        if (sc.uc_intr) nic_error();
        return -1;
    }
    return 0;
}

static int start_fw(int type) {
    WR(IWM_CSR_INT, ~0u);
    if (nic_init()) { LOG("could not init the NIC"); return -1; }
    WR(IWM_CSR_UCODE_DRV_GP1_CLR, IWM_CSR_UCODE_SW_BIT_RFKILL);
    WR(IWM_CSR_UCODE_DRV_GP1_CLR, IWM_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    WR(IWM_CSR_INT, ~0u);
    enable_fwload_interrupt();
    WR(IWM_CSR_UCODE_DRV_GP1_CLR, IWM_CSR_UCODE_SW_BIT_RFKILL);
    WR(IWM_CSR_UCODE_DRV_GP1_CLR, IWM_CSR_UCODE_SW_BIT_RFKILL);
    return load_firmware(type);
}

/* ---- TX queues --------------------------------------------------------------------- */
static int enable_ac_txq(int qid, int fifo) {
    WR(IWM_HBUS_TARG_WRPTR, (u32)(qid << 8 | 0));
    prph_write(IWM_SCD_QUEUE_STATUS_BITS(qid), (0 << IWM_SCD_QUEUE_STTS_REG_POS_ACTIVE) | (1 << IWM_SCD_QUEUE_STTS_REG_POS_SCD_ACT_EN));
    if (clear_bits_prph(IWM_SCD_AGGR_SEL, 1u << qid)) return -1;
    prph_write(IWM_SCD_QUEUE_RDPTR(qid), 0);
    write_mem32(sc.sched_base + IWM_SCD_CONTEXT_QUEUE_OFFSET(qid), 0);
    write_mem32(sc.sched_base + IWM_SCD_CONTEXT_QUEUE_OFFSET(qid) + 4,
                ((IWM_FRAME_LIMIT << IWM_SCD_QUEUE_CTX_REG2_WIN_SIZE_POS) & IWM_SCD_QUEUE_CTX_REG2_WIN_SIZE_MSK) |
                ((IWM_FRAME_LIMIT << IWM_SCD_QUEUE_CTX_REG2_FRAME_LIMIT_POS) & IWM_SCD_QUEUE_CTX_REG2_FRAME_LIMIT_MSK));
    prph_write(IWM_SCD_QUEUE_STATUS_BITS(qid), (1 << IWM_SCD_QUEUE_STTS_REG_POS_ACTIVE) |
               ((u32)fifo << IWM_SCD_QUEUE_STTS_REG_POS_TXF) | (1 << IWM_SCD_QUEUE_STTS_REG_POS_WSL) |
               IWM_SCD_QUEUE_STTS_REG_MSK);
    if (qid == sc.cmdqid) prph_write(IWM_SCD_EN_CTRL, prph_read(IWM_SCD_EN_CTRL) | (1u << qid));
    return 0;
}

static int enable_txq(int sta_id, int qid, int fifo, u8 tid) {
    txring_t *r = &sc.txq[qid];
    WR(IWM_HBUS_TARG_WRPTR, (u32)(qid << 8 | 0));
    r->cur = r->tail = 0;
    struct iwm_scd_txq_cfg_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.tid = tid;
    cmd.scd_queue = (u8)qid;
    cmd.enable = 1;
    cmd.sta_id = (u8)sta_id;
    cmd.tx_fifo = (u8)fifo;
    cmd.aggregate = 0;
    cmd.ssn = 0;
    cmd.window = IWM_FRAME_LIMIT;
    return send_cmd_pdu(IWM_SCD_QUEUE_CFG, 0, sizeof cmd, &cmd);
}

static int post_alive(void) {
    if (!nic_lock()) return -1;
    u32 base = prph_read(IWM_SCD_SRAM_BASE_ADDR);
    if (base != sc.sched_base) LOG("scheduler base: alive says 0x%x, SCD_SRAM_BASE_ADDR 0x%x", sc.sched_base, base);
    disable_interrupts();
    WR(IWM_CSR_INT, ~0u);
    enable_interrupts();
    nic_unlock();
    int nwords = (IWM_SCD_TRANS_TBL_MEM_UPPER_BOUND - IWM_SCD_CONTEXT_MEM_LOWER_BOUND) / 4;
    if (write_mem(sc.sched_base + IWM_SCD_CONTEXT_MEM_LOWER_BOUND, NULL, nwords)) return -1;
    if (!nic_lock()) return -1;
    prph_write(IWM_SCD_DRAM_BASE_ADDR, (u32)(sc.sched.pa >> 10));
    prph_write(IWM_SCD_CHAINEXT_EN, 0);
    int err = enable_ac_txq(sc.cmdqid, IWM_TX_FIFO_CMD);
    if (!err) {
        prph_write(IWM_SCD_TXFACT, 0xff);
        for (int ch = 0; ch < IWM_FH_TCSR_CHNL_NUM; ch++)
            WR(IWM_FH_TCSR_CHNL_TX_CONFIG_REG(ch),
               IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE | IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE);
        SETB(IWM_FH_TX_CHICKEN_BITS_REG, IWM_FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN);
    }
    nic_unlock();
    return err;
}

/* ---- firmware paging (the 8000 family runs part of its code from host memory) ---- */
static int save_fw_paging(int type) {
    if (!sc.paging[0].va) {
        int pages = (int)(sc.fws[type].paging_mem_size / IWM_FW_PAGING_SIZE);
        sc.num_paging_blk = (pages - 1) / IWM_NUM_OF_PAGE_PER_GROUP + 1;
        sc.pages_in_last_blk = pages - IWM_NUM_OF_PAGE_PER_GROUP * (sc.num_paging_blk - 1);
        sc.paging[0] = dma_new(IWM_FW_PAGING_SIZE);
        for (int b = 1; b <= sc.num_paging_blk; b++) sc.paging[b] = dma_new(IWM_PAGING_BLOCK_SIZE);
        for (int b = 0; b <= sc.num_paging_blk; b++) if (!sc.paging[b].va) return -1;
    }
    int s;
    for (s = 0; s < IWM_UCODE_SECT_MAX; s++)
        if (sc.fws[type].sect[s].devoff == IWM_PAGING_SEPARATOR_SECTION) { s++; break; }
    if (s >= IWM_UCODE_SECT_MAX - 1) { LOG("paging: CSS or paging section missing"); return -1; }
    memcpy(sc.paging[0].va, sc.fws[type].sect[s].data, IWM_FW_PAGING_SIZE);        /* CSS block */
    s++;
    u32 off = 0;
    int b;
    for (b = 1; b < sc.num_paging_blk; b++) {
        memcpy(sc.paging[b].va, sc.fws[type].sect[s].data + off, IWM_PAGING_BLOCK_SIZE);
        off += IWM_PAGING_BLOCK_SIZE;
    }
    if (sc.pages_in_last_blk > 0)
        memcpy(sc.paging[b].va, sc.fws[type].sect[s].data + off, (usize)IWM_FW_PAGING_SIZE * sc.pages_in_last_blk);
    return 0;
}

static int send_paging_cmd(void) {
    struct iwm_fw_paging_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.flags = IWM_PAGING_CMD_IS_SECURED | IWM_PAGING_CMD_IS_ENABLED |
                ((u32)sc.pages_in_last_blk << IWM_PAGING_CMD_NUM_OF_PAGES_IN_LAST_GRP_POS);
    cmd.block_size = IWM_BLOCK_2_EXP_SIZE;
    cmd.block_num = (u32)sc.num_paging_blk;
    for (int b = 0; b <= sc.num_paging_blk; b++) cmd.device_phy_addr[b] = (u32)(sc.paging[b].pa >> IWM_PAGE_2_EXP_SIZE);
    return send_cmd_pdu(iwm_cmd_id(IWM_FW_PAGING_BLOCK_CMD, IWM_LONG_GROUP, 0), 0, sizeof cmd, &cmd);
}

static int load_ucode_wait_alive(int type) {
    sc.cmdqid = IWM_DQA_CMD_QUEUE;
    sc.uc_current = type;
    if (start_fw(type)) return -1;
    if (post_alive()) { LOG("post-alive setup failed"); return -1; }
    if (sc.fws[type].paging_mem_size) {
        if (save_fw_paging(type) || send_paging_cmd()) { LOG("firmware paging setup failed"); return -1; }
    }
    return 0;
}

/* ---- NVM (MAC address, antennas, channels) ------------------------------------------ */
static int nvm_read_chunk(u16 section, u16 offset, u16 length, u8 *data, u16 *got) {
    struct iwm_nvm_access_cmd c;
    memset(&c, 0, sizeof c);
    c.offset = offset;
    c.length = length;
    c.type = section;
    c.op_code = 0;                                    /* read */
    hcmd_t h = { .id = IWM_NVM_ACCESS_CMD, .flags = CMD_WANT_RESP, .data = { &c }, .len = { sizeof c },
                 .resp_len = IWM_CMD_RESP_MAX };
    if (send_cmd(&h)) return -1;
    int err = 0;
    struct iwm_nvm_access_resp *r = (void *)h.resp->data;
    if ((h.resp->hdr.flags & IWM_CMD_FAILED_MSK) || r->status || r->offset != offset || r->length > length) err = -1;
    else { memcpy(data + offset, r->data, r->length); *got = r->length; }
    kfree(h.resp);
    return err;
}

static int nvm_read_section(u16 section, u8 *data, u16 *len, usize max) {
    u16 chunk = 2048, seg = 2048;
    *len = 0;
    while (seg == chunk && *len + chunk <= max) {
        if (nvm_read_chunk(section, *len, chunk, data, &seg)) return *len ? 0 : -1;
        *len = (u16)(*len + seg);
    }
    return 0;
}

static const u8 nvm_channels_8000[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92,
    96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177, 181
};

static int nvm_init(void) {
    static const int wanted[] = { IWM_NVM_SECTION_TYPE_HW, IWM_NVM_SECTION_TYPE_SW, IWM_NVM_SECTION_TYPE_REGULATORY,
        IWM_NVM_SECTION_TYPE_CALIBRATION, IWM_NVM_SECTION_TYPE_PRODUCTION, IWM_NVM_SECTION_TYPE_REGULATORY_SDP,
        IWM_NVM_SECTION_TYPE_HW_8000, IWM_NVM_SECTION_TYPE_MAC_OVERRIDE, IWM_NVM_SECTION_TYPE_PHY_SKU };
    struct { u8 *data; u16 len; } sec[IWM_NVM_NUM_OF_SECTIONS];
    memset(sec, 0, sizeof sec);
    usize bufsz = 32768;
    u8 *buf = kalloc(bufsz);
    for (usize i = 0; i < ARRAY_LEN(wanted); i++) {
        u16 len;
        if (nvm_read_section((u16)wanted[i], buf, &len, bufsz) || !len) continue;
        sec[wanted[i]].data = kalloc(len);
        memcpy(sec[wanted[i]].data, buf, len);
        sec[wanted[i]].len = len;
    }
    kfree(buf);
    int err = 0;
    const u16 *sw = (const u16 *)sec[IWM_NVM_SECTION_TYPE_SW].data;
    const u16 *reg = (const u16 *)sec[IWM_NVM_SECTION_TYPE_REGULATORY].data;
    const u16 *hw = (const u16 *)sec[IWM_NVM_SECTION_TYPE_HW_8000].data;
    const u16 *mao = (const u16 *)sec[IWM_NVM_SECTION_TYPE_MAC_OVERRIDE].data;
    const u16 *sku = (const u16 *)sec[IWM_NVM_SECTION_TYPE_PHY_SKU].data;
    if (!sw || !reg || !sku || (!hw && !mao)) {
        LOG("NVM incomplete: sw %d regulatory %d phy-sku %d hw %d mac-override %d", !!sw, !!reg, !!sku, !!hw, !!mao);
        err = -1;
    } else {
        sc.nvm_version = sw[IWM_NVM_VERSION];
        u32 radio = *(const u32 *)(sku + IWM_RADIO_CFG_8000);
        sc.valid_tx_ant = (u8)IWM_NVM_RF_CFG_TX_ANT_MSK_8000(radio);
        sc.valid_rx_ant = (u8)IWM_NVM_RF_CFG_RX_ANT_MSK_8000(radio);
        u32 skuv = *(const u32 *)(sku + IWM_SKU_8000);
        sc.band_5ghz = !!(skuv & IWM_NVM_SKU_CAP_BAND_52GHZ);
        u16 lar_off = sc.nvm_version < 0xE39 ? IWM_NVM_LAR_OFFSET_8000_OLD : IWM_NVM_LAR_OFFSET_8000;
        if ((usize)lar_off * 2 < sec[IWM_NVM_SECTION_TYPE_REGULATORY].len)
            sc.lar_enabled = !!(reg[lar_off] & IWM_NVM_LAR_ENABLED_8000);

        /* MAC address: the override section unless it holds a reserved value, else the OTP registers */
        int have = 0;
        if (mao) {
            static const u8 reserved[6] = { 0x02, 0xcc, 0xaa, 0xff, 0xee, 0x00 };
            const u8 *a = (const u8 *)(mao + IWM_MAC_ADDRESS_OVERRIDE_8000);
            static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, zero[6] = { 0 };
            if (memcmp(a, reserved, 6) && memcmp(a, bcast, 6) && memcmp(a, zero, 6) && !(a[0] & 1)) { memcpy(sc.macaddr, a, 6); have = 1; }
        }
        if (!have && hw && nic_lock()) {
            u32 m0 = prph_read(IWM_WFMP_MAC_ADDR_0), m1 = prph_read(IWM_WFMP_MAC_ADDR_1);
            nic_unlock();
            const u8 *a = (const u8 *)&m0, *b = (const u8 *)&m1;
            sc.macaddr[0] = a[3]; sc.macaddr[1] = a[2]; sc.macaddr[2] = a[1]; sc.macaddr[3] = a[0];
            sc.macaddr[4] = b[1]; sc.macaddr[5] = b[0];
            have = 1;
        }
        /* channels the regulatory section enables */
        sc.n_channels = 0;
        int nreg = sec[IWM_NVM_SECTION_TYPE_REGULATORY].len / 2;
        for (usize i = 0; i < ARRAY_LEN(nvm_channels_8000) && (int)(IWM_NVM_CHANNELS_8000 + i) < nreg; i++) {
            u16 f = reg[IWM_NVM_CHANNELS_8000 + i];
            if (i >= 14 && !sc.band_5ghz) continue;
            if (!(f & IWM_NVM_CHANNEL_VALID)) continue;
            if (sc.n_channels >= (int)sizeof sc.channels || sc.n_channels >= sc.n_scan_channels) break;
            sc.passive[sc.n_channels] = !(f & IWM_NVM_CHANNEL_ACTIVE);
            sc.channels[sc.n_channels++] = nvm_channels_8000[i];
        }
        LOG("NVM v%x: MAC %02x:%02x:%02x:%02x:%02x:%02x, antennas tx %x rx %x, 5 GHz %s, LAR %s, %d channels",
            sc.nvm_version, sc.macaddr[0], sc.macaddr[1], sc.macaddr[2], sc.macaddr[3], sc.macaddr[4], sc.macaddr[5],
            sc.valid_tx_ant, sc.valid_rx_ant, sc.band_5ghz ? "yes" : "no", sc.lar_enabled ? "on" : "off", sc.n_channels);
    }
    for (int i = 0; i < IWM_NVM_NUM_OF_SECTIONS; i++) if (sec[i].data) kfree(sec[i].data);
    return err;
}

static u8 valid_tx_ant(void) {
    u8 a = (u8)((sc.fw_phy_config & IWM_FW_PHY_CFG_TX_CHAIN) >> IWM_FW_PHY_CFG_TX_CHAIN_POS);
    return sc.valid_tx_ant ? a & sc.valid_tx_ant : a;
}
static u8 valid_rx_ant(void) {
    u8 a = (u8)((sc.fw_phy_config & IWM_FW_PHY_CFG_RX_CHAIN) >> IWM_FW_PHY_CFG_RX_CHAIN_POS);
    return sc.valid_rx_ant ? a & sc.valid_rx_ant : a;
}

/* ---- init-image calibration and the regular image's setup (iwm_init_hw) ---------- */
static int send_tx_ant_cfg(void) {
    struct iwm_tx_ant_cfg_cmd c = { .valid = valid_tx_ant() };
    return send_cmd_pdu(IWM_TX_ANT_CONFIGURATION_CMD, 0, sizeof c, &c);
}

static int send_phy_cfg(void) {
    struct iwm_phy_cfg_cmd c;
    memset(&c, 0, sizeof c);
    c.phy_cfg = sc.fw_phy_config;
    c.calib_control.event_trigger = sc.calib[sc.uc_current].event;
    c.calib_control.flow_trigger = sc.calib[sc.uc_current].flow;
    return send_cmd_pdu(IWM_PHY_CONFIGURATION_CMD, 0, sizeof c, &c);
}

static const u32 sf_full_timeout_def[IWM_SF_NUM_SCENARIO][IWM_SF_NUM_TIMEOUT_TYPES] = {
    { IWM_SF_SINGLE_UNICAST_AGING_TIMER_DEF, IWM_SF_SINGLE_UNICAST_IDLE_TIMER_DEF },
    { IWM_SF_AGG_UNICAST_AGING_TIMER_DEF, IWM_SF_AGG_UNICAST_IDLE_TIMER_DEF },
    { IWM_SF_MCAST_AGING_TIMER_DEF, IWM_SF_MCAST_IDLE_TIMER_DEF },
    { IWM_SF_BA_AGING_TIMER_DEF, IWM_SF_BA_IDLE_TIMER_DEF },
    { IWM_SF_TX_RE_AGING_TIMER_DEF, IWM_SF_TX_RE_IDLE_TIMER_DEF },
};
static const u32 sf_full_timeout[IWM_SF_NUM_SCENARIO][IWM_SF_NUM_TIMEOUT_TYPES] = {
    { IWM_SF_SINGLE_UNICAST_AGING_TIMER, IWM_SF_SINGLE_UNICAST_IDLE_TIMER },
    { IWM_SF_AGG_UNICAST_AGING_TIMER, IWM_SF_AGG_UNICAST_IDLE_TIMER },
    { IWM_SF_MCAST_AGING_TIMER, IWM_SF_MCAST_IDLE_TIMER },
    { IWM_SF_BA_AGING_TIMER, IWM_SF_BA_IDLE_TIMER },
    { IWM_SF_TX_RE_AGING_TIMER, IWM_SF_TX_RE_IDLE_TIMER },
};

static int sf_config(int state, int associated) {
    struct iwm_sf_cfg_cmd c;
    memset(&c, 0, sizeof c);
    c.state = (u32)state;
    c.watermark[IWM_SF_LONG_DELAY_ON] = IWM_SF_W_MARK_SCAN;
    c.watermark[IWM_SF_FULL_ON] = associated ? IWM_SF_W_MARK_LEGACY : IWM_SF_W_MARK_MIMO2;
    for (int i = 0; i < IWM_SF_NUM_SCENARIO; i++)
        for (int j = 0; j < IWM_SF_NUM_TIMEOUT_TYPES; j++) c.long_delay_timeouts[i][j] = IWM_SF_LONG_DELAY_AGING_TIMER;
    memcpy(c.full_on_timeouts, associated ? sf_full_timeout : sf_full_timeout_def, sizeof c.full_on_timeouts);
    return send_cmd_pdu(IWM_REPLY_SF_CFG_CMD, CMD_ASYNC, sizeof c, &c);
}

static int run_init_ucode(int justnvm) {
    sc.init_complete = 0;
    if (load_ucode_wait_alive(UC_INIT)) { LOG("init firmware failed to start"); return -1; }
    if (justnvm) return nvm_init();
    if (sf_config(IWM_SF_INIT_OFF, 0) || send_tx_ant_cfg() || send_phy_cfg()) return -1;
    if (wait_for(&sc.init_complete, IWM_INIT_COMPLETE | IWM_CALIB_COMPLETE, 3000)) {
        LOG("calibration did not complete (flags %x)", sc.init_complete);
        return -1;
    }
    return 0;
}

static int send_phy_db_cmd(u16 type, u16 len, const void *data) {
    struct iwm_phy_db_cmd c;
    c.type = type;
    c.length = len;
    hcmd_t h = { .id = IWM_PHY_DB_CMD, .flags = CMD_ASYNC, .data = { &c, data }, .len = { sizeof c, len } };
    return send_cmd(&h);
}

static int send_phy_db_data(void) {
    if (!sc.phydb_cfg.data || !sc.phydb_nch.data) { LOG("calibration results missing"); return -1; }
    if (send_phy_db_cmd(IWM_PHY_DB_CFG, sc.phydb_cfg.size, sc.phydb_cfg.data)) return -1;
    if (send_phy_db_cmd(IWM_PHY_DB_CALIB_NCH, sc.phydb_nch.size, sc.phydb_nch.data)) return -1;
    for (int i = 0; i < IWM_NUM_PAPD_CH_GROUPS; i++)
        if (sc.phydb_papd[i].size) { if (send_phy_db_cmd(IWM_PHY_DB_CALIB_CHG_PAPD, sc.phydb_papd[i].size, sc.phydb_papd[i].data)) return -1; DELAY(1000); }
    for (int i = 0; i < IWM_NUM_TXP_CH_GROUPS; i++)
        if (sc.phydb_txp[i].size) { if (send_phy_db_cmd(IWM_PHY_DB_CALIB_CHG_TXP, sc.phydb_txp[i].size, sc.phydb_txp[i].data)) return -1; DELAY(1000); }
    return 0;
}

static int add_aux_sta(void) {
    if (enable_txq(IWM_AUX_STA_ID, AUX_QUEUE, IWM_TX_FIFO_MCAST, IWM_MAX_TID_COUNT)) return -1;
    struct iwm_add_sta_cmd c;
    memset(&c, 0, sizeof c);
    c.sta_id = IWM_AUX_STA_ID;
    c.station_type = IWM_STA_AUX_ACTIVITY;
    c.mac_id_n_color = IWM_FW_CMD_ID_AND_COLOR(IWM_MAC_INDEX_AUX, 0);
    c.tfd_queue_msk = 1u << AUX_QUEUE;
    c.tid_disable_tx = 0xffff;
    u32 status = IWM_ADD_STA_SUCCESS;
    if (send_cmd_status(IWM_ADD_STA, sizeof c, &c, &status)) return -1;
    return (status & IWM_ADD_STA_STATUS_MASK) == IWM_ADD_STA_SUCCESS ? 0 : -1;
}

static int phy_ctxt_cmd(int id, int channel, u32 action) {
    struct iwm_phy_context_cmd c;
    memset(&c, 0, sizeof c);
    c.id_and_color = IWM_FW_CMD_ID_AND_COLOR(id, 0);
    c.action = action;
    c.apply_time = 0;
    c.ci.band = channel <= 14 ? IWM_PHY_BAND_24 : IWM_PHY_BAND_5;
    c.ci.channel = (u8)channel;
    c.ci.width = IWM_PHY_VHT_CHANNEL_MODE20;
    c.ci.ctrl_pos = IWM_PHY_VHT_CTRL_POS_1_BELOW;
    c.rxchain_info = ((u32)valid_rx_ant() << IWM_PHY_RX_CHAIN_VALID_POS) |
                     (1u << IWM_PHY_RX_CHAIN_CNT_POS) | (1u << IWM_PHY_RX_CHAIN_MIMO_CNT_POS);
    c.txchain_info = valid_tx_ant();
    return send_cmd_pdu(IWM_PHY_CONTEXT_CMD, 0, sizeof c, &c);
}

static int send_update_mcc(const char *alpha2) {
    if (!sc.lar_enabled) return 0;
    struct iwm_mcc_update_cmd c;
    memset(&c, 0, sizeof c);
    c.mcc = (u16)(alpha2[0] << 8 | alpha2[1]);
    c.source_id = api(IWM_UCODE_TLV_API_WIFI_MCC_UPDATE) ? IWM_MCC_SOURCE_GET_CURRENT : IWM_MCC_SOURCE_OLD_FW;
    u16 len = capa(IWM_UCODE_TLV_CAPA_LAR_SUPPORT_V3) ? sizeof(struct iwm_mcc_update_cmd) : sizeof(struct iwm_mcc_update_cmd_v1);
    hcmd_t h = { .id = IWM_MCC_UPDATE_CMD, .flags = CMD_WANT_RESP, .data = { &c }, .len = { len }, .resp_len = IWM_CMD_RESP_MAX };
    if (send_cmd(&h)) return -1;
    int err = (h.resp->hdr.flags & IWM_CMD_FAILED_MSK) ? -1 : 0;
    kfree(h.resp);
    return err;
}

static int config_umac_scan(void) {
    static const u32 rates = IWM_SCAN_CONFIG_RATE_1M | IWM_SCAN_CONFIG_RATE_2M | IWM_SCAN_CONFIG_RATE_5M |
        IWM_SCAN_CONFIG_RATE_11M | IWM_SCAN_CONFIG_RATE_6M | IWM_SCAN_CONFIG_RATE_9M | IWM_SCAN_CONFIG_RATE_12M |
        IWM_SCAN_CONFIG_RATE_18M | IWM_SCAN_CONFIG_RATE_24M | IWM_SCAN_CONFIG_RATE_36M | IWM_SCAN_CONFIG_RATE_48M |
        IWM_SCAN_CONFIG_RATE_54M;
    usize size = sizeof(struct iwm_scan_config) + (usize)sc.n_scan_channels;
    struct iwm_scan_config *c = kalloc(size);
    c->tx_chains = valid_tx_ant();
    c->rx_chains = valid_rx_ant();
    c->legacy_rates = rates | IWM_SCAN_CONFIG_SUPPORTED_RATE(rates);
    c->dwell_active = 10;
    c->dwell_passive = 110;
    c->dwell_fragmented = 44;
    c->dwell_extended = 90;
    c->out_of_channel_time = 0;
    c->suspend_time = 0;
    memcpy(c->mac_addr, sc.macaddr, 6);
    c->bcast_sta_id = IWM_AUX_STA_ID;
    c->channel_flags = 0;
    int n = 0;
    for (; n < sc.n_channels && n < sc.n_scan_channels; n++) c->channel_array[n] = sc.channels[n];
    c->flags = IWM_SCAN_CONFIG_FLAG_ACTIVATE | IWM_SCAN_CONFIG_FLAG_ALLOW_CHUB_REQS |
               IWM_SCAN_CONFIG_FLAG_SET_TX_CHAINS | IWM_SCAN_CONFIG_FLAG_SET_RX_CHAINS |
               IWM_SCAN_CONFIG_FLAG_SET_AUX_STA_ID | IWM_SCAN_CONFIG_FLAG_SET_ALL_TIMES |
               IWM_SCAN_CONFIG_FLAG_SET_LEGACY_RATES | IWM_SCAN_CONFIG_FLAG_SET_MAC_ADDR |
               IWM_SCAN_CONFIG_FLAG_SET_CHANNEL_FLAGS | IWM_SCAN_CONFIG_N_CHANNELS(n) |
               IWM_SCAN_CONFIG_FLAG_CLEAR_FRAGMENTED;
    hcmd_t h = { .id = iwm_cmd_id(IWM_SCAN_CFG_CMD, IWM_LONG_GROUP, 0), .data = { c }, .len = { (u16)size } };
    int err = send_cmd(&h);
    kfree(c);
    return err;
}

static int init_hw(void) {
    if (run_init_ucode(0)) return -1;
    stop_device();
    if (start_hw()) { LOG("could not restart the hardware"); return -1; }
    if (load_ucode_wait_alive(UC_REGULAR)) { LOG("regular firmware failed to start"); return -1; }
    LOG("regular firmware alive");
    if (!nic_lock()) return -1;
    int err = 0;
#define STEP(what, call) if (!err && (call)) { LOG("init: %s failed", what); err = -1; }
    STEP("tx antennas", send_tx_ant_cfg());
    STEP("phy db", send_phy_db_data());
    STEP("phy config", send_phy_cfg());
    if (!err) {
        struct iwm_bt_coex_cmd bt = { .mode = IWM_BT_COEX_WIFI, .enabled_modules = IWM_BT_COEX_HIGH_BAND_RET };
        STEP("bt coex", send_cmd_pdu(IWM_BT_CONFIG, 0, sizeof bt, &bt));
    }
    if (!err) {
        struct iwm_dqa_enable_cmd dqa = { .cmd_queue = IWM_DQA_CMD_QUEUE };
        STEP("dqa", send_cmd_pdu(iwm_cmd_id(IWM_DQA_ENABLE_CMD, IWM_DATA_PATH_GROUP, 0), 0, sizeof dqa, &dqa));
    }
    STEP("aux station", add_aux_sta());
    for (int i = 0; i < IWM_NUM_PHY_CTX && !err; i++)
        STEP("phy context", phy_ctxt_cmd(i, sc.channels[0] ? sc.channels[0] : 1, IWM_FW_CTXT_ACTION_ADD));
    sc.phy_channel = sc.channels[0] ? sc.channels[0] : 1;
    if (!err && capa(IWM_UCODE_TLV_CAPA_CT_KILL_BY_FW)) {
        struct iwm_temp_report_ths_cmd t;
        memset(&t, 0, sizeof t);
        STEP("temperature thresholds", send_cmd_pdu(IWM_WIDE_ID(IWM_PHY_OPS_GROUP, IWM_TEMP_REPORTING_THRESHOLDS_CMD), 0, sizeof t, &t));
    }
    if (!err) {
        struct iwm_device_power_cmd p;
        memset(&p, 0, sizeof p);
        p.flags = IWM_DEVICE_POWER_FLAGS_POWER_SAVE_ENA_MSK;
        STEP("device power", send_cmd_pdu(IWM_POWER_TABLE_CMD, 0, sizeof p, &p));
    }
    if (!err && capa(IWM_UCODE_TLV_CAPA_LAR_SUPPORT)) STEP("regulatory (MCC ZZ)", send_update_mcc("ZZ"));
    STEP("scan config", config_umac_scan());
    static const int ac_fifo[4] = { IWM_TX_FIFO_BE, IWM_TX_FIFO_BK, IWM_TX_FIFO_VI, IWM_TX_FIFO_VO };
    for (int ac = 0; ac < 4 && !err; ac++)
        STEP("tx queue", enable_txq(IWM_STATION_ID, IWM_DQA_MIN_MGMT_QUEUE + ac, ac_fifo[ac], IWM_TID_NON_QOS));
    if (!err) {
        struct iwm_beacon_filter_cmd bf;
        memset(&bf, 0, sizeof bf);
        STEP("beacon filter", send_cmd_pdu(IWM_REPLY_BEACON_FILTERING_CMD, 0, sizeof bf, &bf));
    }
#undef STEP
    nic_unlock();
    return err;
}

/* ---- scanning (iwm_umac_scan) ------------------------------------------------------ */
static u8 *add_rates(u8 *p, const u8 *rates, int n) {
    int first = MIN(n, 8);
    *p++ = 1; *p++ = (u8)first;                      /* Supported Rates */
    memcpy(p, rates, (usize)first); p += first;
    if (n > 8) { *p++ = 50; *p++ = (u8)(n - 8); memcpy(p, rates + 8, (usize)(n - 8)); p += n - 8; }   /* Extended */
    return p;
}

static const u8 rates_11g[] = { 2 | 0x80, 4 | 0x80, 11 | 0x80, 22 | 0x80, 12, 18, 24, 36, 48, 72, 96, 108 };
static const u8 rates_11a[] = { 12 | 0x80, 18, 24 | 0x80, 36, 48 | 0x80, 72, 96, 108 };

static void fill_probe_req(struct iwm_scan_probe_req_v1 *preq) {
    memset(preq, 0, sizeof *preq);
    struct ieee80211_frame *wh = (void *)preq->buf;
    wh->i_fc[0] = 0x40;                               /* management, probe request */
    wh->i_fc[1] = 0;
    memset(wh->i_addr1, 0xff, 6);
    memcpy(wh->i_addr2, sc.macaddr, 6);
    memset(wh->i_addr3, 0xff, 6);
    u8 *frm = (u8 *)(wh + 1);
    *frm++ = 0; *frm++ = 0;                           /* SSID element: the firmware inserts it */
    preq->mac_header.offset = 0;
    preq->mac_header.len = (u16)(frm - preq->buf);
    u8 *pos = frm;
    preq->band_data[0].offset = (u16)(frm - preq->buf);
    frm = add_rates(frm, rates_11g, (int)sizeof rates_11g);
    if (capa(IWM_UCODE_TLV_CAPA_DS_PARAM_SET_IE_SUPPORT)) { *frm++ = 3; *frm++ = 1; *frm++ = 0; }
    preq->band_data[0].len = (u16)(frm - pos);
    if (sc.band_5ghz) {
        pos = frm;
        preq->band_data[1].offset = (u16)(frm - preq->buf);
        frm = add_rates(frm, rates_11a, (int)sizeof rates_11a);
        preq->band_data[1].len = (u16)(frm - pos);
    }
    preq->common_data.offset = (u16)(frm - preq->buf);
    preq->common_data.len = 0;
}

int iwm_scan(void) {
    if (!sc.ready || sc.fatal) return -1;
    if (sc.scanning) return 0;
    usize n = (usize)sc.n_scan_channels;
    usize len = IWM_SCAN_REQ_UMAC_SIZE_V7 + sizeof(struct iwm_scan_channel_cfg_umac) * n + sizeof(struct iwm_scan_req_umac_tail_v1);
    struct iwm_scan_req_umac *req = kalloc(len);
    req->v7.adwell_default_n_aps_social = 10;
    req->v7.adwell_default_n_aps = 2;
    req->v7.adwell_max_budget = 300;
    req->v7.scan_priority = IWM_SCAN_PRIORITY_HIGH;
    req->v7.active_dwell = 10;
    req->v7.passive_dwell = 110;
    req->v7.fragmented_dwell = 44;
    req->ooc_priority = IWM_SCAN_PRIORITY_HIGH;
    struct iwm_scan_channel_cfg_umac *ch = (void *)req->v7.data;
    int nch = 0;
    for (int i = 0; i < sc.n_channels && nch < sc.n_scan_channels; i++, nch++) {
        ch[nch].channel_num = sc.channels[i];
        ch[nch].iter_count = 1;
        ch[nch].iter_interval = 0;
        ch[nch].flags = 0;
    }
    req->v7.channel.count = (u8)nch;
    req->v7.channel.flags = 0;
    struct iwm_scan_req_umac_tail_v1 *tail = (void *)((u8 *)req->v7.data + sizeof(struct iwm_scan_channel_cfg_umac) * n);
    req->general_flags = IWM_UMAC_SCAN_GEN_FLAGS_PASS_ALL | IWM_UMAC_SCAN_GEN_FLAGS_ITER_COMPLETE |
                         IWM_UMAC_SCAN_GEN_FLAGS_PASSIVE | IWM_UMAC_SCAN_GEN_FLAGS_ADAPTIVE_DWELL;
    fill_probe_req(&tail->preq);
    tail->schedule[0].interval = 0;
    tail->schedule[0].iter_count = 1;
    hcmd_t h = { .id = iwm_cmd_id(IWM_SCAN_REQ_UMAC, IWM_LONG_GROUP, 0), .data = { req }, .len = { (u16)len } };
    sc.scanning = 1;
    int err = send_cmd(&h);
    kfree(req);
    if (err) { sc.scanning = 0; LOG("scan request failed"); return -1; }
    LOG("scanning %d channels", nch);
    return 0;
}

int iwm_scanning(void) { return sc.scanning; }
void iwm_scan_forget(void) { sc.scanning = 0; }
u32 iwm_missed_beacons(void) { return sc.missed_beacons; }

/* ---- association: contexts, binding, station (iwm_auth / iwm_run) ------------------ */
/* ack rates (iwm_ack_rates) from the AP's basic rates */
static void ack_rates(const iwm_bss_t *b, u32 *cck_out, u32 *ofdm_out) {
    u32 cck = 0, ofdm = 0;
    int lowest_cck = -1, lowest_ofdm = -1;
    static const u8 cck_rates[] = { 2, 4, 11, 22 }, ofdm_rates[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    for (int j = 0; j < b->n_basic; j++) {
        u8 v = b->basic_rates[j] & 0x7f;
        for (int i = 0; i < 4; i++) if (b->channel <= 14 && v == cck_rates[i]) { cck |= 1u << i; if (lowest_cck < 0 || i < lowest_cck) lowest_cck = i; }
        for (int i = 0; i < 8; i++) if (v == ofdm_rates[i]) { ofdm |= 1u << i; if (lowest_ofdm < 0 || i < lowest_ofdm) lowest_ofdm = i; }
    }
    if (lowest_ofdm < 0) lowest_ofdm = 8;
    if (lowest_cck < 0) lowest_cck = 4;
    if (4 < lowest_ofdm) ofdm |= 1u << 4;         /* 24M */
    if (2 < lowest_ofdm) ofdm |= 1u << 2;         /* 12M */
    ofdm |= 1u << 0;                               /* 6M */
    if (3 < lowest_cck) cck |= 1u << 3;
    if (2 < lowest_cck) cck |= 1u << 2;
    if (1 < lowest_cck) cck |= 1u << 1;
    cck |= 1u << 0;
    *cck_out = cck;
    *ofdm_out = ofdm;
}

static iwm_bss_t cur_bss;

static int mac_ctxt_cmd(u32 action, int assoc) {
    const iwm_bss_t *b = &cur_bss;
    struct iwm_mac_ctx_cmd c;
    memset(&c, 0, sizeof c);
    c.id_and_color = IWM_FW_CMD_ID_AND_COLOR(0, 0);
    c.action = action;
    c.mac_type = IWM_FW_MAC_TYPE_BSS_STA;
    c.tsf_id = IWM_TSF_ID_A;
    memcpy(c.node_addr, sc.macaddr, 6);
    memcpy(c.bssid_addr, b->bssid, 6);
    u32 cck, ofdm;
    ack_rates(b, &cck, &ofdm);
    c.cck_rates = cck;
    c.ofdm_rates = ofdm;
    c.cck_short_preamble = b->short_preamble ? IWM_MAC_FLG_SHORT_PREAMBLE : 0;
    c.short_slot = b->short_slot ? IWM_MAC_FLG_SHORT_SLOT : 0;
    /* 802.11 default EDCA parameters (non-QoS BSS): ecwmin, ecwmax, aifsn, txop (32 us units) */
    static const struct { u8 ecwmin, ecwmax, aifsn; u16 txop; } edca[4] = {
        { 4, 10, 3, 0 }, { 4, 10, 7, 0 }, { 3, 4, 2, 94 }, { 2, 3, 2, 47 } };        /* BE, BK, VI, VO */
    static const int fifo[4] = { IWM_TX_FIFO_BE, IWM_TX_FIFO_BK, IWM_TX_FIFO_VI, IWM_TX_FIFO_VO };
    for (int i = 0; i < 4; i++) {
        int f = fifo[i];
        c.ac[f].cw_min = (u16)((1 << edca[i].ecwmin) - 1);
        c.ac[f].cw_max = (u16)((1 << edca[i].ecwmax) - 1);
        c.ac[f].aifsn = edca[i].aifsn;
        c.ac[f].fifos_mask = (u8)(1 << f);
        c.ac[f].edca_txop = (u16)(edca[i].txop * 32);
    }
    c.filter_flags = IWM_MAC_FILTER_ACCEPT_GRP;
    if (!assoc || !b->assoc_id || !b->dtim_period) {
        c.filter_flags |= IWM_MAC_FILTER_IN_BEACON;
    } else {
        u32 bi = b->beacon_int ? b->beacon_int : 100;
        c.sta.is_assoc = 1;
        c.sta.dtim_time = b->rstamp;
        c.sta.dtim_tsf = b->tsf;
        c.sta.bi = bi;
        c.sta.bi_reciprocal = iwm_reciprocal(bi);
        c.sta.dtim_interval = bi * b->dtim_period;
        c.sta.dtim_reciprocal = iwm_reciprocal(c.sta.dtim_interval);
        c.sta.listen_interval = 10;
        c.sta.assoc_id = b->assoc_id;
        c.sta.assoc_beacon_arrive_time = b->rstamp;
    }
    return send_cmd_pdu(IWM_MAC_CONTEXT_CMD, 0, sizeof c, &c);
}

static int binding_cmd(u32 action) {
    struct iwm_binding_cmd c;
    memset(&c, 0, sizeof c);
    c.id_and_color = IWM_FW_CMD_ID_AND_COLOR(0, 0);
    c.action = action;
    c.phy = IWM_FW_CMD_ID_AND_COLOR(0, 0);
    c.macs[0] = IWM_FW_CMD_ID_AND_COLOR(0, 0);
    for (int i = 1; i < IWM_MAX_MACS_IN_BINDING; i++) c.macs[i] = IWM_FW_CTXT_INVALID;
    c.lmac_id = IWM_LMAC_24G_INDEX;
    u32 status = 0;
    if (send_cmd_status(IWM_BINDING_CONTEXT_CMD, sizeof(struct iwm_binding_cmd_v1), &c, &status)) return -1;
    return status ? -1 : 0;
}

static int add_sta_cmd(int update) {
    struct iwm_add_sta_cmd c;
    memset(&c, 0, sizeof c);
    c.sta_id = IWM_STATION_ID;
    c.station_type = IWM_STA_LINK;
    c.mac_id_n_color = IWM_FW_CMD_ID_AND_COLOR(0, 0);
    c.tfd_queue_msk = 0;
    for (int ac = 0; ac < 4; ac++) c.tfd_queue_msk |= 1u << (IWM_DQA_MIN_MGMT_QUEUE + ac);
    if (!update) memcpy(c.addr, cur_bss.bssid, 6);
    c.add_modify = update ? 1 : 0;
    c.station_flags_msk = IWM_STA_FLG_FAT_EN_MSK | IWM_STA_FLG_MIMO_EN_MSK;
    if (update) c.modify_mask = IWM_STA_MODIFY_QUEUES | IWM_STA_MODIFY_TID_DISABLE_TX;
    c.tid_disable_tx = 0xffff;
    u32 status = IWM_ADD_STA_SUCCESS;
    if (send_cmd_status(IWM_ADD_STA, sizeof c, &c, &status)) return -1;
    return (status & IWM_ADD_STA_STATUS_MASK) == IWM_ADD_STA_SUCCESS ? 0 : -1;
}

/* the firmware's rate table for our station: legacy rates the AP supports, fastest first */
static int set_rates(void) {
    struct iwm_lq_cmd lq;
    memset(&lq, 0, sizeof lq);
    lq.sta_id = IWM_STATION_ID;
    static const struct { u8 rval, plcp; int cck; } tab[] = {
        { 108, IWM_RATE_54M_PLCP, 0 }, { 96, IWM_RATE_48M_PLCP, 0 }, { 72, IWM_RATE_36M_PLCP, 0 },
        { 48, IWM_RATE_24M_PLCP, 0 }, { 36, IWM_RATE_18M_PLCP, 0 }, { 24, IWM_RATE_12M_PLCP, 0 },
        { 22, IWM_RATE_11M_PLCP, 1 }, { 18, IWM_RATE_9M_PLCP, 0 }, { 12, IWM_RATE_6M_PLCP, 0 },
        { 11, IWM_RATE_5M_PLCP, 1 }, { 4, IWM_RATE_2M_PLCP, 1 }, { 2, IWM_RATE_1M_PLCP, 1 } };
    int j = 0;
    u32 lowest = 0;
    for (usize i = 0; i < ARRAY_LEN(tab) && j < (int)ARRAY_LEN(lq.rs_table); i++) {
        if (tab[i].cck && cur_bss.channel > 14) continue;
        int ok = 0;
        for (int k = 0; k < cur_bss.n_rates; k++) if ((cur_bss.rates[k] & 0x7f) == tab[i].rval) ok = 1;
        if (!ok) continue;
        u32 t = tab[i].plcp | IWM_RATE_MCS_ANT_A_MSK | (tab[i].cck ? IWM_RATE_MCS_CCK_MSK : 0);
        lq.rs_table[j++] = t;
        lowest = t;
    }
    if (!j) lowest = cur_bss.channel > 14 ? (IWM_RATE_6M_PLCP | IWM_RATE_MCS_ANT_A_MSK)
                                          : (IWM_RATE_1M_PLCP | IWM_RATE_MCS_ANT_A_MSK | IWM_RATE_MCS_CCK_MSK);
    while (j < (int)ARRAY_LEN(lq.rs_table)) lq.rs_table[j++] = lowest;
    lq.single_stream_ant_msk = IWM_ANT_A;
    lq.dual_stream_ant_msk = IWM_ANT_AB;
    lq.agg_time_limit = 4000;
    lq.agg_disable_start_th = 3;
    lq.agg_frame_cnt_limit = 0x3f;
    return send_cmd_pdu(IWM_LQ_CMD, 0, sizeof lq, &lq);
}

static int protect_session(u32 duration, u32 max_delay) {
    struct iwm_time_event_cmd t;
    memset(&t, 0, sizeof t);
    t.action = IWM_FW_CTXT_ACTION_ADD;
    t.id_and_color = IWM_FW_CMD_ID_AND_COLOR(0, 0);
    t.id = IWM_TE_BSS_STA_AGGRESSIVE_ASSOC;
    t.apply_time = 0;
    t.max_frags = IWM_TE_V2_FRAG_NONE;
    t.max_delay = max_delay;
    t.interval = 1;
    t.duration = duration;
    t.repeat = 1;
    t.policy = IWM_TE_V2_NOTIF_HOST_EVENT_START | IWM_TE_V2_NOTIF_HOST_EVENT_END | IWM_T2_V2_START_IMMEDIATELY;
    hcmd_t h = { .id = IWM_TIME_EVENT_CMD, .flags = CMD_WANT_RESP, .data = { &t }, .len = { sizeof t },
                 .resp_len = sizeof(struct iwm_rx_packet) + sizeof(struct iwm_time_event_resp) };
    if (send_cmd(&h)) return -1;
    int err = -1;
    if (!(h.resp->hdr.flags & IWM_CMD_FAILED_MSK)) {
        struct iwm_time_event_resp *r = (void *)h.resp->data;
        if (r->status == 0) { sc.te_uid = r->unique_id; sc.te_active = 1; err = 0; }
    }
    kfree(h.resp);
    return err;
}

int iwm_auth_prepare(const iwm_bss_t *b) {
    if (!sc.ready || sc.fatal) return -1;
    if (sc.mac_active) iwm_disconnect();
    cur_bss = *b;
    memcpy(sc.bssid, b->bssid, 6);
    sc.missed_beacons = 0;
    sc.have_ptk = 0;
    sc.tsc = 0;
    if (phy_ctxt_cmd(0, b->channel, IWM_FW_CTXT_ACTION_MODIFY)) { LOG("could not tune to channel %d", b->channel); return -1; }
    sc.phy_channel = b->channel;
    set_rates();
    if (mac_ctxt_cmd(IWM_FW_CTXT_ACTION_ADD, 0)) { LOG("could not add the MAC context"); return -1; }
    sc.mac_active = 1;
    if (binding_cmd(IWM_FW_CTXT_ACTION_ADD)) { LOG("could not add the binding"); return -1; }
    sc.binding_active = 1;
    if (add_sta_cmd(0)) { LOG("could not add the station"); return -1; }
    sc.sta_active = 1;
    u32 bi = b->beacon_int ? b->beacon_int : 100;
    if (protect_session(bi * 2, bi / 2)) LOG("could not schedule the association time event");
    LOG("ready to authenticate with %02x:%02x:%02x:%02x:%02x:%02x on channel %d",
        b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5], b->channel);
    return 0;
}

int iwm_assoc_done(const iwm_bss_t *b) {
    cur_bss = *b;
    int err = 0;
    if (add_sta_cmd(1)) { LOG("station update failed"); err = -1; }
    if (!err && mac_ctxt_cmd(IWM_FW_CTXT_ACTION_MODIFY, 1)) { LOG("MAC update failed"); err = -1; }
    if (!err && sf_config(IWM_SF_FULL_ON, 1)) err = -1;
    if (!err) {
        usize size = (sizeof(struct iwm_mcast_filter_cmd) + 3) & ~3u;
        struct iwm_mcast_filter_cmd *m = kalloc(size);
        m->filter_own = 1;
        m->port_id = 0;
        m->count = 0;
        m->pass_all = 1;
        memcpy(m->bssid, b->bssid, 6);
        if (send_cmd_pdu(IWM_MCAST_FILTER_CMD, 0, (u16)size, m)) { LOG("multicast filter failed"); err = -1; }
        kfree(m);
    }
    if (!err) {
        struct iwm_mac_power_cmd p;
        memset(&p, 0, sizeof p);
        p.id_and_color = IWM_FW_CMD_ID_AND_COLOR(0, 0);
        u32 dtim_ms = (u32)(b->dtim_period ? b->dtim_period : 1) * (b->beacon_int ? b->beacon_int : 100);
        u32 ka = MAX(3 * dtim_ms, 1000u * IWM_POWER_KEEP_ALIVE_PERIOD_SEC);
        p.keep_alive_seconds = (u16)((ka + 999) / 1000);
        p.flags = IWM_POWER_FLAGS_POWER_SAVE_ENA_MSK;
        if (send_cmd_pdu(IWM_MAC_PM_POWER_TABLE, 0, sizeof p, &p)) { LOG("power table failed"); err = -1; }
    }
    if (!err && !capa(IWM_UCODE_TLV_CAPA_DYNAMIC_QUOTA)) {
        struct iwm_time_quota_cmd q;
        memset(&q, 0, sizeof q);
        for (int i = 0; i < IWM_MAX_BINDINGS; i++) q.quotas[i].id_and_color = IWM_FW_CTXT_INVALID;
        q.quotas[0].id_and_color = IWM_FW_CMD_ID_AND_COLOR(0, 0);
        q.quotas[0].quota = IWM_MAX_QUOTA;
        q.quotas[0].max_duration = 0;
        u16 len = api(IWM_UCODE_TLV_API_QUOTA_LOW_LATENCY) ? sizeof q : sizeof(struct iwm_time_quota_cmd_v1);
        if (!api(IWM_UCODE_TLV_API_QUOTA_LOW_LATENCY)) {
            struct iwm_time_quota_cmd_v1 q1;
            memset(&q1, 0, sizeof q1);
            for (int i = 0; i < IWM_MAX_BINDINGS; i++) {
                q1.quotas[i].id_and_color = q.quotas[i].id_and_color;
                q1.quotas[i].quota = q.quotas[i].quota;
                q1.quotas[i].max_duration = q.quotas[i].max_duration;
            }
            if (send_cmd_pdu(IWM_TIME_QUOTA_CMD, 0, len, &q1)) err = -1;
        } else if (send_cmd_pdu(IWM_TIME_QUOTA_CMD, 0, len, &q)) err = -1;
        if (err) LOG("time quota failed");
    }
    if (!err) set_rates();
    if (!err) LOG("associated: AID %u, DTIM period %u, beacon interval %u TU", b->assoc_id & 0x3fff, b->dtim_period, b->beacon_int);
    return err;
}

void iwm_disconnect(void) {
    if (sc.sta_active) {
        struct iwm_rm_sta_cmd r;
        memset(&r, 0, sizeof r);
        r.sta_id = IWM_STATION_ID;
        send_cmd_pdu(IWM_REMOVE_STA, 0, sizeof r, &r);
        sc.sta_active = 0;
    }
    if (sc.binding_active) { binding_cmd(IWM_FW_CTXT_ACTION_REMOVE); sc.binding_active = 0; }
    if (sc.mac_active) { mac_ctxt_cmd(IWM_FW_CTXT_ACTION_REMOVE, 0); sc.mac_active = 0; }
    sc.have_ptk = 0;
    sc.te_active = 0;
}

int iwm_set_pairwise_key(const u8 key[16]) {
    struct iwm_add_sta_key_cmd_v1 c;
    memset(&c, 0, sizeof c);
    c.common.key_flags = IWM_STA_KEY_FLG_CCM | IWM_STA_KEY_FLG_WEP_KEY_MAP |
                         ((0 << IWM_STA_KEY_FLG_KEYID_POS) & IWM_STA_KEY_FLG_KEYID_MSK);
    memcpy(c.common.key, key, 16);
    c.common.key_offset = 0;
    c.common.sta_id = IWM_STATION_ID;
    if (send_cmd_pdu(IWM_ADD_STA_KEY, 0, sizeof c, &c)) { LOG("could not install the pairwise key"); return -1; }
    memcpy(sc.ptk_tk, key, 16);
    sc.have_ptk = 1;
    sc.tsc = 0;
    LOG("pairwise key installed (CCMP in hardware)");
    return 0;
}

/* ---- transmit (iwm_tx) -------------------------------------------------------------- */
#define PAYLOAD_OFF 1024

int iwm_tx(const u8 *frame, usize len, int mgmt) {
    if (!sc.ready || sc.fatal || len < 24) return -1;
    txring_t *r = &sc.txq[DATA_QUEUE];
    if (r->queued >= IWM_TX_RING_COUNT - 16) { service(); if (r->queued >= IWM_TX_RING_COUNT - 16) return -1; }
    const struct ieee80211_frame *wh0 = (const void *)frame;
    u32 hdrlen = 24;
    u8 type = wh0->i_fc[0] & 0x0c, subtype = wh0->i_fc[0] & 0xf0;
    int multicast = wh0->i_addr1[0] & 1;
    int protect = (wh0->i_fc[1] & 0x40) && !multicast && sc.have_ptk && type == 0x08;
    usize body = len - hdrlen;
    if (body + 16 > SLOT - PAYLOAD_OFF) return -1;

    int idx = r->cur;
    u8 *slot = r->slots.va + (usize)idx * SLOT;
    u64 pa = r->slots.pa + (u64)idx * SLOT;
    memset(slot, 0, PAYLOAD_OFF);
    struct iwm_cmd_header *ch = (void *)slot;
    ch->code = IWM_TX_CMD;
    ch->flags = 0;
    ch->qid = (u8)r->qid;
    ch->idx = (u8)idx;
    struct iwm_tx_cmd *tx = (void *)(slot + sizeof *ch);

    tx->rts_retry_limit = IWM_RTS_DFAULT_RETRY_LIMIT;
    tx->data_retry_limit = IWM_LOW_RETRY_LIMIT;
    if (multicast || type != 0x08) {
        /* management and group frames at the lowest rate */
        u32 rate = sc.phy_channel > 14 ? IWM_RATE_6M_PLCP : (IWM_RATE_1M_PLCP | IWM_RATE_MCS_CCK_MSK);
        tx->rate_n_flags = rate | IWM_RATE_MCS_ANT_A_MSK;
        tx->data_retry_limit = IWM_MGMT_DFAULT_RETRY_LIMIT;
    } else {
        tx->initial_rate_index = 0;
        tx->tx_flags |= IWM_TX_CMD_FLG_STA_RATE;
    }
    u32 flags = 0;
    if (!multicast) flags |= IWM_TX_CMD_FLG_ACK;
    tx->sta_id = IWM_STATION_ID;
    if (type == 0x00) tx->pm_frame_timeout = (subtype == 0x00 || subtype == 0x20) ? 3 : 2;   /* (re)assoc request */
    else tx->pm_frame_timeout = 0;
    u32 totlen = (u32)len;
    u8 *hdr = (u8 *)tx + sizeof *tx;
    memcpy(hdr, frame, hdrlen);
    struct ieee80211_frame *wh = (void *)hdr;
    /* non-QoS: the driver numbers frames (IWM_TX_CMD_FLG_SEQ_CTL) */
    u16 seq = (u16)(sc.seq++ << 4);
    wh->i_seq[0] = (u8)seq; wh->i_seq[1] = (u8)(seq >> 8);
    u8 *pl = slot + PAYLOAD_OFF;
    u32 plen = (u32)body;
    if (protect) {
        sc.tsc++;
        pl[0] = (u8)sc.tsc; pl[1] = (u8)(sc.tsc >> 8); pl[2] = 0; pl[3] = 0x20;     /* ExtIV, key 0 */
        pl[4] = (u8)(sc.tsc >> 16); pl[5] = (u8)(sc.tsc >> 24); pl[6] = (u8)(sc.tsc >> 32); pl[7] = (u8)(sc.tsc >> 40);
        memcpy(pl + 8, frame + hdrlen, body);
        plen += 8;
        totlen += 8;
        tx->sec_ctl = IWM_TX_CMD_SEC_CCM;
        memcpy(tx->key, sc.ptk_tk, 16);
    } else {
        memcpy(pl, frame + hdrlen, body);
        tx->sec_ctl = 0;
    }
    flags |= IWM_TX_CMD_FLG_BT_DIS | IWM_TX_CMD_FLG_SEQ_CTL;
    tx->tx_flags |= flags;
    tx->len = (u16)totlen;
    tx->tid_tspec = type == 0x08 ? IWM_TID_NON_QOS : IWM_MAX_TID_COUNT;
    tx->life_time = IWM_TX_CMD_LIFE_TIME_INFINITE;
    u64 scratch = pa + sizeof *ch + __builtin_offsetof(struct iwm_tx_cmd, scratch);
    tx->dram_lsb_ptr = (u32)scratch;
    tx->dram_msb_ptr = iwm_get_dma_hi_addr(scratch);

    struct iwm_tfd *desc = (struct iwm_tfd *)r->desc.va + idx;
    memset(desc, 0, sizeof *desc);
    u32 cmdlen = sizeof *ch + sizeof *tx + hdrlen;           /* 24-byte header: no pad needed */
    set_tb(&desc->tbs[0], pa, 16);
    set_tb(&desc->tbs[1], pa + 16, cmdlen - 16);
    set_tb(&desc->tbs[2], pa + PAYLOAD_OFF, plen);
    desc->num_tbs = 3;
    update_sched(r->qid, idx, IWM_STATION_ID, (u16)(totlen + (protect ? 8 : 0)));
    barrier();
    r->used[idx] = 1;
    r->queued++;
    r->cur = (r->cur + 1) % IWM_TX_RING_COUNT;
    WR(IWM_HBUS_TARG_WRPTR, (u32)(r->qid << 8 | r->cur));
    sc.tx_frames++;
    (void)mgmt;
    return 0;
}

/* ---- attach / start / poll -------------------------------------------------------- */
int iwm_probe(pci_dev_t *d) {
    if (d->vendor != 0x8086) return 0;
    if (d->device == 0x24f3 || d->device == 0x24f4) { sc.pci = d; sc.present = 1; return 1; }   /* Wireless 8260 */
    return 0;
}

int iwm_present(void) { return sc.present; }

/* Without a card (QEMU), still check that the firmware file is on the stick and parses. */
void iwm_check_firmware(void) {
    static int done;
    if (done || sc.present) return;
    done = 1;
    if (read_firmware() == 0) LOG("firmware file checked: %s, ready for an Intel Wireless 8260", sc.fwver);
}
int iwm_ready(void) { return sc.ready && !sc.fatal; }
const u8 *iwm_macaddr(void) { return sc.macaddr; }
const char *iwm_fw_version(void) { return sc.fwver; }
void iwm_set_rx(iwm_rx_fn fn) { sc.rx = fn; }

const char *iwm_status(void) {
    if (!sc.present) return "no supported Intel wireless card";
    if (sc.fatal) fmt(sc.status, sizeof sc.status, "stopped after an error (see the log)");
    else if (!sc.attached) fmt(sc.status, sizeof sc.status, "Intel Wireless 8260: not started");
    else fmt(sc.status, sizeof sc.status, "Intel Wireless 8260, firmware %s, %s; %llu frames in, %llu out, %llu failed",
             sc.fwver, sc.ready ? "running" : "idle", sc.rx_frames, sc.tx_frames, sc.tx_fail);
    return sc.status;
}

int iwm_attach(void) {
    if (!sc.present) return -1;
    if (sc.attached) return 0;
    pci_dev_t *p = sc.pci;
    u32 cmd = pci_read32(p->bus, p->dev, p->fn, 0x04);
    /* memory space + bus master, INTx disabled (QRT polls) */
    pci_write32(p->bus, p->dev, p->fn, 0x04, (cmd & 0xffff) | 0x0006 | 0x0400);
    /* disable the RETRY_TIMEOUT register (0x41), as iwm does */
    u32 r40 = pci_read32(p->bus, p->dev, p->fn, 0x40);
    pci_write32(p->bus, p->dev, p->fn, 0x40, r40 & ~0xff00u);
    /* D0 through the power management capability */
    int pm = pci_find_cap(p->bus, p->dev, p->fn, 0x01);
    if (pm) {
        u32 pmcsr = pci_read32(p->bus, p->dev, p->fn, (u16)(pm + 4));
        if (pmcsr & 3) { pci_write32(p->bus, p->dev, p->fn, (u16)(pm + 4), pmcsr & ~3u); DELAY(10000); LOG("woke the card from D%u", pmcsr & 3); }
    }
    u64 bar = pci_bar(p->bus, p->dev, p->fn, 0);
    if (!bar) { LOG("BAR0 not assigned"); return -1; }
    sc.regs = (volatile u8 *)(usize)bar;
    sc.hw_rev = RD(IWM_CSR_HW_REV);
    LOG("Intel Wireless 8260 at %02x:%02x.%x, registers at 0x%llx, HW_REV %08x", p->bus, p->dev, p->fn, bar, sc.hw_rev);
    if (sc.hw_rev == 0xffffffffu) { LOG("the card does not answer (powered off?)"); return -1; }

    /* 8000 family: revision step moved; C step is read from the AUX bus */
    sc.hw_rev = (sc.hw_rev & 0xfff0) | (IWM_CSR_HW_REV_STEP(sc.hw_rev << 2) << 2);
    if (prepare_card_hw()) return -1;
    SETB(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    DELAY(2);
    if (!poll_bit(IWM_CSR_GP_CNTRL, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000)) {
        LOG("failed to wake up the NIC"); return -1;
    }
    if (!nic_lock()) return -1;
    u32 v = prph_read(IWM_WFPM_CTRL_REG);
    prph_write(IWM_WFPM_CTRL_REG, v | IWM_ENABLE_WFPM);
    u32 step = (prph_read(IWM_AUX_MISC_REG) >> IWM_HW_STEP_LOCATION_BITS) & 0xf;
    if (step == 0x3) sc.hw_rev = (sc.hw_rev & 0xFFFFFFF3) | (IWM_SILICON_C_STEP << 2);
    nic_unlock();

    if (read_firmware()) return -1;
    if (alloc_rings()) { LOG("out of DMA memory"); return -1; }
    WR(IWM_CSR_INT, 0xffffffffu);
    if (rfkill()) LOG("the radio is switched off (rfkill)");

    /* preinit: run the init image once to read the NVM */
    if (start_hw()) return -1;
    int err = run_init_ucode(1);
    stop_device();
    if (err) { LOG("could not read the NVM"); return -1; }
    sc.attached = 1;
    return 0;
}

int iwm_start(void) {
    if (!sc.attached && iwm_attach()) return -1;
    if (sc.ready) return 0;
    sc.fatal = 0;
    if (rfkill()) { LOG("radio switched off"); return -1; }
    if (start_hw()) return -1;
    u64 t0 = k_now_ms();
    if (init_hw()) { stop_device(); return -1; }
    sc.ready = 1;
    LOG("ready in %llu ms", k_now_ms() - t0);
    return 0;
}

void iwm_stop(void) {
    if (!sc.attached) return;
    iwm_disconnect();
    stop_device();
    sc.ready = 0;
    sc.scanning = 0;
}

void iwm_poll(void) {
    if (!sc.attached || !sc.regs) return;
    service();
}
