/*
 * dwi2c.c - Synopsys DesignWare I2C master, polled.
 *
 * Written from the DesignWare DW_apb_i2c register interface.  The firmware
 * has already powered, clocked and timed the controller (it drives the
 * touchscreen through it), so we keep its speed and SCL settings and only
 * program the target address per transfer.  Every transfer runs at
 * TPL_HIGH_LEVEL so a firmware timer callback can never interleave with it.
 */
#include "dwi2c.h"

enum {
    IC_CON = 0x00, IC_TAR = 0x04, IC_DATA_CMD = 0x10, IC_INTR_MASK = 0x30,
    IC_RAW_INTR_STAT = 0x34, IC_CLR_INTR = 0x40, IC_CLR_TX_ABRT = 0x54,
    IC_CLR_STOP_DET = 0x60, IC_ENABLE = 0x6c, IC_STATUS = 0x70, IC_TXFLR = 0x74,
    IC_RXFLR = 0x78, IC_TX_ABRT_SOURCE = 0x80, IC_ENABLE_STATUS = 0x9c,
    IC_COMP_PARAM_1 = 0xf4, IC_COMP_VERSION = 0xf8, IC_COMP_TYPE = 0xfc,
};
#define DW_COMP_TYPE_VALUE 0x44570140u   /* "DW" + 0x0140 */
#define ST_TFNF   (1u << 1)
#define ST_RFNE   (1u << 3)
#define ST_MACT   (1u << 5)
#define RAW_TX_ABRT  (1u << 6)
#define RAW_STOP_DET (1u << 9)
#define CMD_READ    (1u << 8)
#define CMD_STOP    (1u << 9)
#define CMD_RESTART (1u << 10)

static EFI_GUID pci_guid = PCI_IO_GUID;

static inline u32 rd(dwi2c_t *c, u32 off) { return *(volatile u32 *)(c->base + off); }
static inline void wr(dwi2c_t *c, u32 off, u32 v) { *(volatile u32 *)(c->base + off) = v; }

int dwi2c_find(dwi2c_t *c, u32 bus, u32 dev, u32 fn) {
    UINTN n = 0;
    EFI_HANDLE *h = NULL;
    memset(c, 0, sizeof *c);
    if (EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &n, &h))) return DW_ENODEV;
    for (UINTN i = 0; i < n; i++) {
        EFI_PCI_IO_PROTOCOL *p;
        UINTN s, b, d, f;
        u32 cfg[6];
        if (EFI_ERROR(k.bs->HandleProtocol(h[i], &pci_guid, (void **)&p))) continue;
        if (EFI_ERROR(p->GetLocation(p, &s, &b, &d, &f)) || b != bus || d != dev || f != fn) continue;
        if (EFI_ERROR(p->Pci.Read(p, 2, 0, 6, cfg))) continue;
        u64 bar = cfg[4] & ~0xfu;
        if ((cfg[4] & 6) == 4) bar |= (u64)cfg[5] << 32;     /* 64-bit BAR */
        if (!bar || (sizeof(void *) == 4 && bar >> 32)) continue;
        c->base = (volatile u8 *)(usize)bar;
        c->pci_handle = h[i];
        c->pci_id = cfg[0];
        break;
    }
    k.bs->FreePool(h);
    if (!c->base) return DW_ENODEV;
    c->comp_type = rd(c, IC_COMP_TYPE);
    if (c->comp_type != DW_COMP_TYPE_VALUE) { c->base = NULL; return DW_ENODEV; }
    c->comp_ver = rd(c, IC_COMP_VERSION);
    c->comp_param = rd(c, IC_COMP_PARAM_1);
    c->rx_depth = ((c->comp_param >> 8) & 0xff) + 1;
    c->tx_depth = ((c->comp_param >> 16) & 0xff) + 1;
    if (c->rx_depth < 2 || c->rx_depth > 256) c->rx_depth = 32;
    if (c->tx_depth < 2 || c->tx_depth > 256) c->tx_depth = 32;
    c->found = 1;
    return DW_OK;
}

static int wait_enable(dwi2c_t *c, int on, u64 deadline) {
    wr(c, IC_ENABLE, on ? 1 : 0);
    while ((rd(c, IC_ENABLE_STATUS) & 1) != (u32)on)
        if (k_now_ms() > deadline) return DW_ETIMEOUT;
    return DW_OK;
}

int dwi2c_xfer(dwi2c_t *c, u8 addr, const u8 *w, int wlen, u8 *r, int rlen) {
    if (!c->found) return DW_ENODEV;
    UINTN old = k.bs->RaiseTPL(TPL_HIGH_LEVEL);
    u64 deadline = k_now_ms() + 50 + (u64)(wlen + rlen) / 8;   /* generous even at 100 kHz */
    int err = DW_OK;

    while (rd(c, IC_STATUS) & ST_MACT)                 /* bus idle */
        if (k_now_ms() > deadline) { err = DW_ETIMEOUT; goto out; }
    if ((err = wait_enable(c, 0, deadline))) goto out;
    wr(c, IC_CON, rd(c, IC_CON) | 1u /* master */ | (1u << 5) /* restart */ | (1u << 6) /* no slave */);
    wr(c, IC_TAR, addr & 0x7f);
    wr(c, IC_INTR_MASK, 0);
    (void)rd(c, IC_CLR_INTR);
    if ((err = wait_enable(c, 1, deadline))) goto out;

    int total = wlen + rlen, sent = 0, got = 0;
    while (got < rlen || sent < total) {
        u32 raw = rd(c, IC_RAW_INTR_STAT);
        if (raw & RAW_TX_ABRT) {
            c->last_abort = rd(c, IC_TX_ABRT_SOURCE);
            (void)rd(c, IC_CLR_TX_ABRT);
            err = DW_EABORT;
            goto disable;
        }
        /* queue commands while the TX FIFO has room and RX cannot overflow */
        while (sent < total && (rd(c, IC_STATUS) & ST_TFNF)) {
            if (sent >= wlen && (sent - wlen) - got >= (int)c->rx_depth - 1) break;
            u32 cmd;
            if (sent < wlen) cmd = w[sent];
            else cmd = CMD_READ | (sent == wlen && wlen ? CMD_RESTART : 0);
            if (sent == total - 1) cmd |= CMD_STOP;
            wr(c, IC_DATA_CMD, cmd);
            sent++;
        }
        while (got < rlen && rd(c, IC_RXFLR)) r[got++] = (u8)rd(c, IC_DATA_CMD);
        if (k_now_ms() > deadline) { err = DW_ETIMEOUT; goto disable; }
    }
    while (!(rd(c, IC_RAW_INTR_STAT) & (RAW_STOP_DET | RAW_TX_ABRT)))
        if (k_now_ms() > deadline) { err = DW_ETIMEOUT; goto disable; }
    if (rd(c, IC_RAW_INTR_STAT) & RAW_TX_ABRT) {
        c->last_abort = rd(c, IC_TX_ABRT_SOURCE);
        (void)rd(c, IC_CLR_TX_ABRT);
        err = DW_EABORT;
    }
    (void)rd(c, IC_CLR_STOP_DET);
disable:
    /* leave the controller enabled, as the firmware expects to find it */
    (void)rd(c, IC_CLR_INTR);
out:
    k.bs->RestoreTPL(old);
    return err;
}

const char *dwi2c_strerror(int err) {
    switch (err) {
    case DW_OK: return "ok";
    case DW_ETIMEOUT: return "timeout";
    case DW_EABORT: return "no ACK (abort)";
    case DW_ENODEV: return "no controller";
    default: return "error";
    }
}
