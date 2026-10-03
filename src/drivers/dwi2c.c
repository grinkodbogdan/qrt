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
#include "pci.h"

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
    memset(c, 0, sizeof *c);
    /* registers: BAR0 read straight from PCI config space (ECAM) */
    if (pci_available()) {
        u64 bar = pci_bar((u8)bus, (u8)dev, (u8)fn, 0);
        if (bar && !(sizeof(void *) == 4 && bar >> 32)) {
            c->base = (volatile u8 *)(usize)bar;
            c->pci_id = pci_read32((u8)bus, (u8)dev, (u8)fn, 0);
        }
    }
    /* while the firmware runs, also find its handle so its driver can be detached */
    if (!k.native) {
        UINTN n = 0;
        EFI_HANDLE *h = NULL;
        if (!EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &n, &h))) {
            for (UINTN i = 0; i < n; i++) {
                EFI_PCI_IO_PROTOCOL *p;
                UINTN s, b, d, f;
                u32 cfg[6];
                if (EFI_ERROR(k.bs->HandleProtocol(h[i], &pci_guid, (void **)&p))) continue;
                if (EFI_ERROR(p->GetLocation(p, &s, &b, &d, &f)) || b != bus || d != dev || f != fn) continue;
                c->pci_handle = h[i];
                if (!c->base && !EFI_ERROR(p->Pci.Read(p, 2, 0, 6, cfg))) {
                    u64 bar = cfg[4] & ~0xfu;
                    if ((cfg[4] & 6) == 4) bar |= (u64)cfg[5] << 32;
                    if (bar && !(sizeof(void *) == 4 && bar >> 32)) { c->base = (volatile u8 *)(usize)bar; c->pci_id = cfg[0]; }
                }
                break;
            }
            k.bs->FreePool(h);
        }
    }
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
    UINTN old = 0;
    usize flags = 0;
    if (k.native) __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    else old = k.bs->RaiseTPL(TPL_HIGH_LEVEL);
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
    if (k.native) { if (flags & 0x200) __asm__ volatile("sti" ::: "memory"); }
    else k.bs->RestoreTPL(old);
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

/*
 * The firmware configured the controller (clocks, SCL timing, speed).  Its
 * drivers stop at ExitBootServices and may reset or power the block down, so
 * the configuration is captured beforehand and written back afterwards.
 */
/*
 * Standard mode (100 kHz) for a slow device.  A bus the firmware never used
 * (I2C3 on the Venue: the battery controller) is still in reset with its clock
 * gated: take it out (LPSS private registers, as Linux's acpi_lpss does) and
 * program the SCL counts for the 100 MHz LPSS clock (Linux's formulas:
 * tHIGH 4.0 us, tLOW 4.7 us + 0.3 us fall).  If the clock is faster the bus
 * only runs slower, which a standard-mode device accepts.
 */
void dwi2c_standard_mode(dwi2c_t *c) {
    if (!c->found) return;
    if ((rd(c, 0x804) & 3) != 3) { wr(c, 0x804, 3); hal_delay_us(100); }
    if (!(rd(c, 0x800) & 1)) wr(c, 0x800, rd(c, 0x800) | 1);
    wr(c, IC_ENABLE, 0);
    for (int i = 0; i < 100000 && (rd(c, IC_ENABLE_STATUS) & 1); i++) {}
    if (rd(c, 0x14) < 100 || rd(c, 0x18) < 100) { wr(c, 0x14, 392); wr(c, 0x18, 499); }   /* IC_SS_SCL_HCNT / LCNT */
    if (!rd(c, 0x7c)) wr(c, 0x7c, 30);                                                      /* SDA hold, 300 ns */
    wr(c, IC_CON, 1u | (1u << 1) | (1u << 5) | (1u << 6));                                  /* master, standard, restart, no slave */
    wr(c, IC_ENABLE, 1);
}

static const u16 saved_regs[] = { 0x00, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x7c, 0x94, 0xa0, 0xa4 };

void dwi2c_save(dwi2c_t *c) {
    if (!c->found) return;
    for (usize i = 0; i < ARRAY_LEN(saved_regs); i++) c->saved[i] = rd(c, saved_regs[i]);
    c->saved_priv[0] = rd(c, 0x800);
    c->saved_priv[1] = rd(c, 0x804);
    c->has_saved = 1;
}

int dwi2c_restore(dwi2c_t *c, u8 fn) {
    if (!c->found || !c->has_saved) return DW_ENODEV;
    if (pci_available()) {
        /* power state D0 and memory decoding on */
        int pm = pci_find_cap(0, 0x18, fn, 0x01);
        if (pm) pci_write32(0, 0x18, fn, (u16)(pm + 4), pci_read32(0, 0x18, fn, (u16)(pm + 4)) & ~3u);
        pci_write32(0, 0x18, fn, 0x04, pci_read32(0, 0x18, fn, 0x04) | 0x6);
    }
    if (rd(c, 0x804) != c->saved_priv[1]) wr(c, 0x804, c->saved_priv[1]);      /* LPSS resets */
    if (rd(c, 0x800) != c->saved_priv[0]) wr(c, 0x800, c->saved_priv[0]);      /* LPSS clock */
    if (rd(c, IC_COMP_TYPE) != DW_COMP_TYPE_VALUE) return DW_ENODEV;
    wr(c, IC_ENABLE, 0);
    for (int i = 0; i < 100000 && (rd(c, IC_ENABLE_STATUS) & 1); i++) {}
    for (usize i = 1; i < ARRAY_LEN(saved_regs); i++) wr(c, saved_regs[i], c->saved[i]);
    wr(c, IC_CON, c->saved[0]);
    wr(c, IC_ENABLE, 1);
    return DW_OK;
}
