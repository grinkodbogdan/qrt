/*
 * ehci.c - USB 2 host controllers (EHCI 1.0), for PCs whose internal USB devices are
 * not on the xHCI controller: on Intel 7-series chipsets (the Panasonic FZ-G1) only
 * four USB 2 ports can be switched to xHCI, the others stay on the two EHCI
 * controllers, each with Intel's "rate-matching" hub on its first port.
 *
 * Written from the EHCI 1.0 specification, with Linux's ehci-hcd and OpenBSD's ehci(4)
 * as references for what it leaves open: the BIOS hand-off, 64-bit data structures
 * (one 4 GB segment for queue heads and transfer descriptors, CTRLDSSEGMENT), the
 * alternate next pointer that lets a short control read reach its status stage, and
 * split transactions for full and low speed devices behind a high speed hub.
 *
 * Polled like xhci.c, and below the same device code (enumeration, hubs, keyboards,
 * mice, touchscreens, Bluetooth: xhci.c and usb.h): each endpoint has one queue head
 * with one transfer descriptor; control and bulk queue heads hang in the asynchronous
 * schedule, interrupt ones in the periodic schedule (every frame).  Isochronous
 * transfers (USB audio) are not supported here.
 */
#include "../../kernel/kernel.h"
#include "../pci.h"
#include "usbint.h"
#include "ehci.h"

#if defined(__x86_64__)
#include "../../arch/x64/mm.h"
#include "../../arch/x64/sched.h"

#define USBCMD      0x00
#define USBSTS      0x04
#define USBINTR     0x08
#define CTRLDSSEG   0x10
#define PERIODICBASE 0x14
#define ASYNCADDR   0x18
#define CONFIGFLAG  0x40
#define PORTSC(n)   (0x44 + 4 * (n))
#define CMD_RUN     (1u << 0)
#define CMD_HCRESET (1u << 1)
#define CMD_PSE     (1u << 4)
#define CMD_ASE     (1u << 5)
#define CMD_IAAD    (1u << 6)
#define STS_IAA     (1u << 5)
#define STS_HALTED  (1u << 12)
#define PS_CCS      (1u << 0)
#define PS_CSC      (1u << 1)
#define PS_PE       (1u << 2)
#define PS_PEC      (1u << 3)
#define PS_OCC      (1u << 5)
#define PS_PR       (1u << 8)
#define PS_PP       (1u << 12)
#define PS_W1C      (PS_CSC | PS_PEC | PS_OCC)

#define TOK_ACTIVE  0x80u
#define TOK_HALTED  0x40u
#define TOK_ERRORS  0x7cu                    /* halted, buffer error, babble, transaction error, missed */
#define PID_OUT 0
#define PID_IN 1
#define PID_SETUP 2

#define BLK 256                              /* pool block: a queue head (68 bytes), or descriptors (+ a setup packet) */
#define POOL_PAGES 128

typedef struct {
    pci_dev_t *pci;
    volatile u8 *op;
    int ports, is64;
    u8 *pool; u64 pool_pa;                   /* frame list (4 KiB), then BLK-sized blocks */
    int pool_next;
    u32 *frames;
    u32 *async_head, *intr_head;
    u32 present;                             /* root ports with a device */
    u32 noted;                               /* root ports whose full-speed device we reported */
    u64 xfers;                               /* IN transfers completed with data */
    char status[96];
} ehc_t;

static ehc_t hc[2];
static int nhc;
static void *free_blocks;

#define LOCK()   u64 lock_fl_ = irq_save()
#define UNLOCK() irq_restore(lock_fl_)

static u32 rd(ehc_t *c, u32 r) { return *(volatile u32 *)(c->op + r); }
static void wr(ehc_t *c, u32 r, u32 v) { *(volatile u32 *)(c->op + r) = v; }
static void mb(void) { __asm__ volatile("mfence" ::: "memory"); }
static u32 lo(const void *p) { return (u32)(u64)(usize)p; }

static int wait_reg(ehc_t *c, u32 r, u32 mask, u32 want, u32 ms) {
    for (u32 i = 0; i < ms * 10; i++) { if ((rd(c, r) & mask) == want) return 1; hal_delay_us(100); }
    return 0;
}

static u32 *block(ehc_t *c) {
    if (free_blocks) { u32 *b = free_blocks; free_blocks = *(void **)b; memset(b, 0, BLK); return b; }
    if ((usize)(c->pool_next + BLK) > POOL_PAGES * 4096u) return NULL;
    u32 *b = (u32 *)(c->pool + c->pool_next);
    c->pool_next += BLK;
    return b;
}
static void unblock(void *b) { if (b) { *(void **)b = free_blocks; free_blocks = b; } }

static ehc_t *ctl_of(udev_t *d) { return &hc[d->ehc]; }

/* ---- queue heads and transfer descriptors (EHCI 3.5, 3.6; appendix B for 64 bits) -------- */
static void qh_setup(u32 *qh, udev_t *d, int ep, int mps, int control, int periodic) {
    u32 eps = d->speed == SPEED_HIGH ? 2 : d->speed == SPEED_LOW ? 1 : 0;
    qh[1] = (u32)(d->slot & 0x7f) | (u32)(ep & 15) << 8 | eps << 12 | (control ? 1u << 14 : 0) | (u32)(mps & 0x7ff) << 16 |
            (control && eps != 2 ? 1u << 27 : 0) | (periodic ? 0 : 8u << 28);
    qh[2] = 1u << 30;                                          /* one transaction per microframe */
    if (eps != 2) qh[2] |= (u32)(d->tt_slot & 0x7f) << 16 | (u32)(d->tt_port & 0x7f) << 23;
    if (periodic) qh[2] |= eps == 2 ? 0x01 : 0x01 | 0x1c << 8; /* start in microframe 0; splits complete in 2..4 */
}
static void qh_init(u32 *qh, udev_t *d, int ep, int mps, int control, int periodic) {
    memset(qh, 0, BLK);
    qh[0] = 1;
    qh_setup(qh, d, ep, mps, control, periodic);
    qh[4] = 1; qh[5] = 1;                                      /* overlay: no next descriptor */
}

static void qtd_fill(u32 *t, u32 next, u32 alt, int pid, int toggle, u64 buf, int len) {
    t[0] = next; t[1] = alt;
    for (int i = 0; i < 5; i++) {
        u64 a = i == 0 ? buf : (buf & ~0xfffull) + (u64)i * 4096;
        t[3 + i] = (u32)a;
        t[8 + i] = (u32)(a >> 32);
    }
    mb();
    t[2] = (u32)toggle << 31 | (u32)len << 16 | 3u << 10 | (u32)pid << 8 | TOK_ACTIVE;
}

/* hand a descriptor chain to an idle queue head */
static void qh_start(u32 *qh, u32 *first) {
    u32 keep_toggle = qh[6] & 0x80000000u;
    qh[6] = keep_toggle;                                       /* not active, not halted */
    qh[3] = 0;
    mb();
    qh[4] = lo(first);
    mb();
}

static void link_async(ehc_t *c, u32 *qh) {
    LOCK();
    qh[0] = c->async_head[0];
    mb();
    c->async_head[0] = lo(qh) | 2;
    UNLOCK();
}
static void link_periodic(ehc_t *c, u32 *qh) {
    LOCK();
    qh[0] = c->intr_head[0];
    mb();
    c->intr_head[0] = lo(qh) | 2;
    UNLOCK();
}
/* take a queue head out of whichever schedule holds it; the controller lets go after a doorbell (async) or a frame */
static void unlink_qh(ehc_t *c, u32 *qh) {
    for (int sched = 0; sched < 2; sched++) {
        u32 *p = sched ? c->intr_head : c->async_head;
        for (int guard = 0; guard < 256; guard++) {
            u32 nx = p[0];
            if ((nx & 1) || (nx & ~0x1fu) == lo(c->async_head)) break;
            u32 *q = (u32 *)(usize)(((u64)(c->pool_pa >> 32) << 32) | (nx & ~0x1fu));
            if (q == qh) {
                p[0] = qh[0];
                mb();
                if (!sched) {
                    wr(c, USBSTS, STS_IAA);
                    wr(c, USBCMD, rd(c, USBCMD) | CMD_IAAD);
                    wait_reg(c, USBSTS, STS_IAA, STS_IAA, 10);
                    wr(c, USBSTS, STS_IAA);
                } else hal_delay_us(2000);
                return;
            }
            p = q;
        }
    }
}

/* ---- control transfers ----------------------------------------------------------------- */
static int wait_qtd(volatile u32 *t, u32 ms) {
    u64 end = k_now_us() + (u64)ms * 1000;
    while (t[2] & TOK_ACTIVE) {
        if (k_now_us() > end) return -1;
        thread_yield();
    }
    return 0;
}

int ehci_address(udev_t *d) {
    ehc_t *c = ctl_of(d);
    static u8 next_addr[2] = { 1, 1 };
    u32 *qh = block(c), *td = block(c);
    if (!qh || !td) return -1;
    d->eqh0 = qh;
    d->eqtd0 = td;
    int addr = 0;
    for (int tries = 0; tries < 127 && !addr; tries++) {       /* the next address no device has */
        int a = next_addr[d->ehc]++;
        if (next_addr[d->ehc] > 127) next_addr[d->ehc] = 1;
        int taken = 0;
        udev_t *all = usb_devices();
        for (int i = 0; i < MAX_DEV; i++) if (all[i].used && all[i].hc && all[i].ehc == d->ehc && &all[i] != d && all[i].slot == a) taken = 1;
        if (!taken) addr = a;
    }
    d->slot = 0;
    qh_init(qh, d, 0, d->mps0, 1, 0);
    link_async(c, qh);
    if (ehci_control(d, 0x00, 5, (u16)addr, 0, NULL, 0)) {    /* SET_ADDRESS */
        unlink_qh(c, qh);
        d->eqh0 = NULL;
        unblock(qh); unblock(td);
        return -1;
    }
    hal_delay_us(2000);                                        /* SET_ADDRESS recovery */
    d->slot = addr;
    qh_setup(qh, d, 0, d->mps0, 1, 0);
    return 0;
}

void ehci_set_mps0(udev_t *d) { if (d->eqh0) qh_setup(d->eqh0, d, 0, d->mps0, 1, 0); }

int ehci_control(udev_t *d, u8 rtype, u8 req, u16 val, u16 idx, void *data, u16 len) {
    u32 *qh = d->eqh0, *t = d->eqtd0;
    if (!qh || !t || len > 4096) return -1;
    u32 *setup_td = t, *data_td = t + 16, *status_td = t + 32;  /* 64 bytes apart; the setup packet at +192 */
    u8 *setup = (u8 *)(t + 48);
    int in = (rtype & 0x80) != 0;
    if (!in && len) memcpy(d->buf, data, len);
    setup[0] = rtype; setup[1] = req;
    setup[2] = (u8)val; setup[3] = (u8)(val >> 8);
    setup[4] = (u8)idx; setup[5] = (u8)(idx >> 8);
    setup[6] = (u8)len; setup[7] = (u8)(len >> 8);
    u64 bufpa = (u64)(usize)d->buf;
    qtd_fill(status_td, 1, 1, in && len ? PID_OUT : PID_IN, 1, bufpa, 0);
    status_td[2] |= 1u << 15;
    if (len) qtd_fill(data_td, lo(status_td), lo(status_td), in ? PID_IN : PID_OUT, 1, bufpa, len);
    qtd_fill(setup_td, len ? lo(data_td) : lo(status_td), 1, PID_SETUP, 0, (u64)(usize)setup, 8);
    LOCK();
    qh_start(qh, setup_td);
    UNLOCK();
    int timeout = wait_qtd(status_td, 1000);
    u32 err = (setup_td[2] | (len ? data_td[2] : 0) | status_td[2]) & TOK_ERRORS;
    if (timeout || (err & TOK_HALTED)) {
        /* take the descriptors back: an idle, unhalted overlay */
        LOCK();
        for (u32 *x = setup_td; x <= status_td; x += 16) x[2] &= ~TOK_ACTIVE;
        qh[4] = 1; qh[6] = 0;
        UNLOCK();
        return timeout ? -1 : -2;
    }
    if (in && len) memcpy(data, d->buf, len);
    return 0;
}

/* ---- bulk and interrupt endpoints ------------------------------------------------------- */
static void submit_in(uep_t *e) {
    e->elen = e->type == 3 ? e->mps : IN_BUF;
    qtd_fill(e->eqtd, 1, 1, PID_IN, 0, (u64)(usize)e->buf, e->elen);
    ((u32 *)e->eqtd)[2] |= 1u << 15;
    qh_start(e->eqh, e->eqtd);
    e->ebusy = 1;
}

int ehci_ep_open(udev_t *d, uep_t *e) {
    ehc_t *c = ctl_of(d);
    u32 *qh = e->eqh ? e->eqh : block(c), *td = e->eqtd ? e->eqtd : block(c);
    if (!qh || !td) return -1;
    int ep = e->dci / 2;                                       /* xhci.c's dci: endpoint number * 2 (+1 for IN) */
    qh_init(qh, d, ep, e->mps, 0, e->type == 3);
    e->eqh = qh; e->eqtd = td;
    if (e->type == 3) link_periodic(c, qh); else link_async(c, qh);
    if (e->in) { LOCK(); submit_in(e); UNLOCK(); }
    return 0;
}

int ehci_bulk_out(udev_t *d, uep_t *e, const void *data, int len) {
    if (!e->eqh) return -1;
    memcpy(e->buf, data, (usize)len);
    LOCK();
    qtd_fill(e->eqtd, 1, 1, PID_OUT, 0, (u64)(usize)e->buf, len);
    qh_start(e->eqh, e->eqtd);
    UNLOCK();
    int timeout = wait_qtd(e->eqtd, 1000);
    u32 tok = ((u32 *)e->eqtd)[2];
    if (timeout || (tok & TOK_HALTED)) {
        LOCK();
        ((u32 *)e->eqtd)[2] &= ~TOK_ACTIVE;
        ((u32 *)e->eqh)[4] = 1; ((u32 *)e->eqh)[6] &= 0x80000000u;
        UNLOCK();
        return -1;
    }
    return 0;
}

void ehci_poll_locked(void) {
    if (!nhc) return;
    udev_t *all = usb_devices();
    for (int i = 0; i < MAX_DEV; i++) {
        udev_t *d = &all[i];
        if (!d->used || !d->hc) continue;
        for (int j = 0; j < MAX_EP; j++) {
            uep_t *e = &d->ep[j];
            if (!e->open || !e->in || !e->ebusy) continue;
            u32 tok = ((volatile u32 *)e->eqtd)[2];
            if (tok & TOK_ACTIVE) continue;
            e->ebusy = 0;
            if (tok & TOK_HALTED) {
                klog("usb: port %d endpoint %02x stopped (token %08x)", d->port, e->dci / 2 | 0x80, tok);
                ((u32 *)e->eqh)[4] = 1; ((u32 *)e->eqh)[6] = 0;
                continue;
            }
            int got = e->elen - (int)((tok >> 16) & 0x7fff);
            if (got > 0 && !hc[d->ehc].xfers++) klog("usb: EHCI %d: first data from port %d (%d bytes)", d->ehc, d->port, got);
            if (got > 0 && e->cb) e->cb(e->arg, e->buf, got);
            if (e->open && !e->ebusy) submit_in(e);
        }
    }
}

void ehci_free_dev(udev_t *d) {
    ehc_t *c = ctl_of(d);
    for (int j = 0; j < MAX_EP; j++) {
        uep_t *e = &d->ep[j];
        if (e->eqh) { unlink_qh(c, e->eqh); unblock(e->eqh); unblock(e->eqtd); }
        e->eqh = e->eqtd = NULL;
        e->ebusy = 0;
    }
    if (d->eqh0) { unlink_qh(c, d->eqh0); unblock(d->eqh0); unblock(d->eqtd0); }
    d->eqh0 = d->eqtd0 = NULL;
}

/* ---- root ports ------------------------------------------------------------------------- */
static void port_scan(int n) {
    ehc_t *c = &hc[n];
    for (int p = 0; p < c->ports; p++) {
        u32 v = rd(c, PORTSC(p));
        if (v & PS_W1C) wr(c, PORTSC(p), (v & ~PS_W1C & ~PS_PE) | (v & PS_W1C));
        int root = 100 * (n + 1) + p + 1;
        udev_t *d = NULL, *all = usb_devices();
        for (int i = 0; i < MAX_DEV; i++) if (all[i].used && all[i].hc && all[i].ehc == n && !all[i].parent && all[i].port == root) d = &all[i];
        if ((v & PS_CCS) && !d) {
            if (((v >> 10) & 3) == 1) {                        /* K state: a low-speed device, no companion controller */
                if (!(c->noted & (1u << p))) klog("usb: EHCI %d port %d: a low-speed device (needs a companion controller)", n, p + 1);
                c->noted |= 1u << p;
                continue;
            }
            wr(c, PORTSC(p), (v & ~PS_W1C & ~PS_PE) | PS_PR);
            thread_sleep_ms(50);
            wr(c, PORTSC(p), rd(c, PORTSC(p)) & ~PS_W1C & ~PS_PR);
            wait_reg(c, PORTSC(p), PS_PR, 0, 10);
            thread_sleep_ms(10);
            v = rd(c, PORTSC(p));
            if (!(v & PS_PE)) {
                if (!(c->noted & (1u << p))) klog("usb: EHCI %d port %d: a full-speed device (needs a companion controller)", n, p + 1);
                c->noted |= 1u << p;
                continue;
            }
            usb_enumerate(1, n, root, SPEED_HIGH, NULL, 0);
        } else if (!(v & PS_CCS) && d) {
            c->noted &= ~(1u << p);
            usb_detach_tree(d);
        } else if (!(v & PS_CCS)) c->noted &= ~(1u << p);
    }
}

void ehci_ports(void) { for (int n = 0; n < nhc; n++) port_scan(n); }
int ehci_count(void) { return nhc; }

/* ---- bring-up ---------------------------------------------------------------------------- */
static void bios_handoff(pci_dev_t *p, u32 hcc) {
    u32 eecp = (hcc >> 8) & 0xff;
    for (int guard = 0; eecp >= 0x40 && guard < 8; guard++) {
        u32 v = pci_read32(p->bus, p->dev, p->fn, (u16)eecp);
        if ((v & 0xff) == 1) {                                 /* USB legacy support */
            pci_write32(p->bus, p->dev, p->fn, (u16)eecp, v | (1u << 24));   /* OS owned */
            for (int i = 0; i < 1000 && (pci_read32(p->bus, p->dev, p->fn, (u16)eecp) & (1u << 16)); i++) hal_delay_us(1000);
            if (pci_read32(p->bus, p->dev, p->fn, (u16)eecp) & (1u << 16)) klog("usb: EHCI: the firmware did not let go");
            pci_write32(p->bus, p->dev, p->fn, (u16)(eecp + 4), 0);          /* SMIs off */
            return;
        }
        eecp = (v >> 8) & 0xff;
    }
}

int ehci_probe(pci_dev_t *p) {
    if (!k.native || nhc >= 2) return 0;
    ehc_t *c = &hc[nhc];
    memset(c, 0, sizeof *c);
    c->pci = p;
    u32 cmd = pci_read32(p->bus, p->dev, p->fn, 4);
    pci_write32(p->bus, p->dev, p->fn, 4, cmd | 0x6);
    u64 bar = pci_bar(p->bus, p->dev, p->fn, 0);
    if (!bar) { strlcpy(c->status, "no register BAR", sizeof c->status); return 0; }
    volatile u8 *cap = mm_map_mmio(bar, 0x1000);
    if (!cap) { strlcpy(c->status, "registers could not be mapped", sizeof c->status); return 0; }
    c->op = cap + cap[0];
    u32 hcs = *(volatile u32 *)(cap + 4), hcc = *(volatile u32 *)(cap + 8);
    c->ports = (int)MIN(hcs & 15, 15u);
    c->is64 = hcc & 1;
    bios_handoff(p, hcc);

    wr(c, USBCMD, rd(c, USBCMD) & ~CMD_RUN);
    if (!wait_reg(c, USBSTS, STS_HALTED, STS_HALTED, 20)) { strlcpy(c->status, "did not halt", sizeof c->status); return 0; }
    wr(c, USBCMD, CMD_HCRESET);
    if (!wait_reg(c, USBCMD, CMD_HCRESET, 0, 250)) { strlcpy(c->status, "reset did not finish", sizeof c->status); return 0; }
    wr(c, USBINTR, 0);

    c->pool = hal_dma_alloc(POOL_PAGES * 4096);
    c->pool_pa = (u64)(usize)c->pool;
    if (!c->pool || (c->pool_pa >> 32) != ((c->pool_pa + POOL_PAGES * 4096 - 1) >> 32) || (!c->is64 && c->pool_pa >> 32)) {
        strlcpy(c->status, "no DMA memory it can reach", sizeof c->status); return 0;
    }
    if (c->is64) wr(c, CTRLDSSEG, (u32)(c->pool_pa >> 32));
    c->frames = (u32 *)c->pool;
    c->pool_next = 4096;
    c->async_head = block(c);
    c->intr_head = block(c);
    /* the asynchronous schedule: a head (H bit) that points at itself */
    c->async_head[0] = lo(c->async_head) | 2;
    c->async_head[1] = 1u << 15;
    c->async_head[4] = 1; c->async_head[5] = 1; c->async_head[6] = TOK_HALTED;
    /* the periodic schedule: every frame starts at a dummy interrupt queue head */
    c->intr_head[0] = 1;
    c->intr_head[4] = 1; c->intr_head[5] = 1;
    for (int i = 0; i < 1024; i++) c->frames[i] = lo(c->intr_head) | 2;
    mb();
    wr(c, PERIODICBASE, lo(c->frames));
    wr(c, ASYNCADDR, lo(c->async_head));
    wr(c, USBCMD, 8u << 16 | CMD_ASE | CMD_PSE | CMD_RUN);   /* 1 ms interrupt threshold, 1024 frames */
    if (!wait_reg(c, USBSTS, STS_HALTED, 0, 20)) { strlcpy(c->status, "did not start", sizeof c->status); return 0; }
    wr(c, CONFIGFLAG, 1);                                      /* every port to this controller */
    hal_delay_us(5000);
    if (hcs & (1u << 4))
        for (int i = 0; i < c->ports; i++) wr(c, PORTSC(i), (rd(c, PORTSC(i)) & ~PS_W1C & ~PS_PE) | PS_PP);
    int n = nhc++;
    hal_delay_us(100000);
    port_scan(n);
    int devs = 0;
    udev_t *all = usb_devices();
    for (int i = 0; i < MAX_DEV; i++) if (all[i].used && all[i].hc && all[i].ehc == n) devs++;
    fmt(c->status, sizeof c->status, "EHCI, %d ports, %s DMA, %d device%s at start", c->ports, c->is64 ? "64-bit" : "32-bit", devs, devs == 1 ? "" : "s");
    klog("usb: EHCI %02x:%02x.%x: %s", p->bus, p->dev, p->fn, c->status);
    return 1;
}

void ehci_status(pci_dev_t *p, char *buf, usize cap) {
    for (int i = 0; i < nhc; i++) if (hc[i].pci == p) { strlcpy(buf, hc[i].status, cap); return; }
    strlcpy(buf, nhc < 2 ? "not started" : "a third controller (two are used)", cap);
}

#else
int ehci_probe(pci_dev_t *p) { (void)p; return 0; }
void ehci_status(pci_dev_t *p, char *buf, usize cap) { (void)p; strlcpy(buf, "needs the 64-bit native kernel", cap); }
#endif
