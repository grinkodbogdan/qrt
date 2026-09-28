/*
 * e1000.c - Intel 8254x / 82574 gigabit Ethernet (QEMU's e1000 and e1000e).
 *
 * QRT's tablet has no Ethernet; this driver exists so the network stack
 * (DHCP, DNS, TCP) can be exercised under QEMU, where it is the default NIC.
 * Legacy descriptors, polled, native mode only (in firmware mode the
 * firmware's own driver may still own the card).
 */
#include "e1000.h"
#include "../net/net.h"
#include "../net/wifilog.h"

enum { CTRL = 0x0000, STATUS = 0x0008, ICR = 0x00c0, IMC = 0x00d8, RCTL = 0x0100, TCTL = 0x0400, TIPG = 0x0410,
       RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818,
       TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818,
       MTA = 0x5200, RAL = 0x5400, RAH = 0x5404 };

#define NRX 64
#define NTX 64
#define BUF 2048

typedef struct { u64 addr; u16 len, csum; u8 status, errors; u16 special; } __attribute__((packed)) rxd_t;
typedef struct { u64 addr; u16 len; u8 cso, cmd, status, css; u16 special; } __attribute__((packed)) txd_t;

static struct {
    pci_dev_t *pci;
    volatile u8 *regs;
    rxd_t *rx; u8 *rxbuf;
    txd_t *tx; u8 *txbuf;
    int rx_cur, tx_cur;
    netif_t nif;
    u64 rx_frames, tx_frames;
    char status[96];
} e;

static u32 rd(u32 r) { return *(volatile u32 *)(e.regs + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(e.regs + r) = v; }

int e1000_probe(pci_dev_t *d) {
    if (d->vendor != 0x8086) return 0;
    static const u16 ids[] = { 0x100e, 0x100f, 0x10d3, 0x153a };
    for (usize i = 0; i < ARRAY_LEN(ids); i++) if (d->device == ids[i]) { e.pci = d; return 1; }
    return 0;
}

static int send(netif_t *n, const u8 *f, usize len) {
    (void)n;
    if (len > BUF) return -1;
    txd_t *d = &e.tx[e.tx_cur];
    if (d->cmd && !(d->status & 1)) return -1;              /* ring full */
    memcpy(e.txbuf + (usize)e.tx_cur * BUF, f, len);
    d->addr = (u64)(usize)(e.txbuf + (usize)e.tx_cur * BUF);
    d->len = (u16)len;
    d->cmd = 0x01 | 0x02 | 0x08;                              /* EOP, insert FCS, report status */
    d->status = 0;
    __asm__ volatile("mfence" ::: "memory");
    e.tx_cur = (e.tx_cur + 1) % NTX;
    wr(TDT, (u32)e.tx_cur);
    e.tx_frames++;
    return 0;
}

int e1000_start(void) {
    if (!e.pci || !k.native) return -1;
    pci_dev_t *p = e.pci;
    u32 cmd = pci_read32(p->bus, p->dev, p->fn, 0x04);
    pci_write32(p->bus, p->dev, p->fn, 0x04, (cmd & 0xffff) | 0x0006 | 0x0400);   /* memory, bus master, no INTx */
    e.regs = (volatile u8 *)(usize)pci_bar(p->bus, p->dev, p->fn, 0);
    if (!e.regs) return -1;
    wr(IMC, 0xffffffffu);
    wr(CTRL, rd(CTRL) | (1u << 26));                          /* reset */
    hal_delay_us(10000);
    for (int i = 0; i < 1000 && (rd(CTRL) & (1u << 26)); i++) hal_delay_us(10);
    wr(IMC, 0xffffffffu);
    (void)rd(ICR);
    wr(CTRL, rd(CTRL) | (1u << 6) | (1u << 5));               /* set link up, auto-speed */
    u32 lo = rd(RAL), hi = rd(RAH);
    for (int i = 0; i < 4; i++) e.nif.mac[i] = (u8)(lo >> (8 * i));
    e.nif.mac[4] = (u8)hi; e.nif.mac[5] = (u8)(hi >> 8);
    wr(RAH, hi | (1u << 31));
    for (int i = 0; i < 128; i++) wr(MTA + 4 * (u32)i, 0);

    e.rx = hal_dma_alloc(NRX * sizeof(rxd_t));
    e.tx = hal_dma_alloc(NTX * sizeof(txd_t));
    e.rxbuf = hal_dma_alloc((usize)NRX * BUF);
    e.txbuf = hal_dma_alloc((usize)NTX * BUF);
    if (!e.rx || !e.tx || !e.rxbuf || !e.txbuf) return -1;
    for (int i = 0; i < NRX; i++) e.rx[i].addr = (u64)(usize)(e.rxbuf + (usize)i * BUF);
    u64 rxa = (u64)(usize)e.rx, txa = (u64)(usize)e.tx;
    wr(RDBAL, (u32)rxa); wr(RDBAH, (u32)(rxa >> 32));
    wr(RDLEN, NRX * sizeof(rxd_t));
    wr(RDH, 0); wr(RDT, NRX - 1);
    wr(RCTL, (1u << 1) | (1u << 15) | (1u << 26));            /* enable, broadcast, strip CRC; 2 KB buffers */
    wr(TDBAL, (u32)txa); wr(TDBAH, (u32)(txa >> 32));
    wr(TDLEN, NTX * sizeof(txd_t));
    wr(TDH, 0); wr(TDT, 0);
    wr(TIPG, 0x0060200a);
    wr(TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));
    e.rx_cur = e.tx_cur = 0;

    e.nif.name = "Ethernet";
    e.nif.send = send;
    net_register(&e.nif);
    wifilog("e1000: %04x:%04x at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x", p->vendor, p->device, p->bus, p->dev, p->fn,
            e.nif.mac[0], e.nif.mac[1], e.nif.mac[2], e.nif.mac[3], e.nif.mac[4], e.nif.mac[5]);
    return 0;
}

void e1000_poll(void) {
    if (!e.regs) return;
    int link = (rd(STATUS) >> 1) & 1;
    if (link != e.nif.link) {
        e.nif.link = link;
        wifilog("e1000: link %s", link ? "up" : "down");
        net_link_changed(&e.nif);
    }
    for (int budget = 0; budget < NRX; budget++) {
        rxd_t *d = &e.rx[e.rx_cur];
        if (!(d->status & 1)) break;
        if ((d->status & 2) && !d->errors) { e.rx_frames++; net_input(&e.nif, e.rxbuf + (usize)e.rx_cur * BUF, d->len); }
        d->status = 0;
        wr(RDT, (u32)e.rx_cur);
        e.rx_cur = (e.rx_cur + 1) % NRX;
    }
}

const char *e1000_status(void) {
    if (!e.pci) return "no card";
    if (!e.regs) return k.native ? "not started" : "left to the firmware";
    fmt(e.status, sizeof e.status, "link %s, %llu frames in, %llu out", e.nif.link ? "up" : "down", e.rx_frames, e.tx_frames);
    return e.status;
}
