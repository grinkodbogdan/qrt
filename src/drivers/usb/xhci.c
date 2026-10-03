/*
 * xhci.c - the USB 3 host controller (xHCI 1.x) and the devices on its root
 * ports.  Cherry Trail has one (PCI 8086:22b5); behind it on the Venue 8 Pro
 * 5855 sit the external USB-C port and the Bluetooth half of the Intel
 * 8260 card.  QEMU's qemu-xhci is the same programming model.
 *
 * Written from the xHCI 1.1 specification, with OpenBSD's xhci(4)
 * (sys/dev/usb/xhci.c, Martin Pieuchot) as the reference for the parts the
 * specification leaves to experience: the BIOS hand-off, Intel's port
 * routing, scratchpad buffers and context sizes.
 *
 * Polled, like QRT's other drivers: one command ring, one event ring
 * (interrupter 0, interrupts off), read each frame from the input path and
 * by any driver waiting for a transfer.  All users run on the boot core, so
 * turning interrupts off around ring and event-ring work is the lock.
 * Devices on root ports are addressed, their descriptors read and listed;
 * class drivers open endpoints through usb.h: boot-protocol HID keyboards
 * (here) and Bluetooth (src/drivers/bt).  USB 2 hubs are enumerated too
 * (their ports are polled with GET_STATUS); USB 3 hubs show up as their
 * USB 2 half on the tablet's USB 2 port.
 */
#include "../../kernel/kernel.h"
#include "xhci.h"
#include "usb.h"
#include "../bt/bt.h"
#include "uaudio.h"
#include "../hidmouse.h"

#if defined(__x86_64__)
#include "../../arch/x64/mm.h"
#include "../../arch/x64/sched.h"

typedef struct { volatile u64 ptr; volatile u32 status, flags; } trb_t;
typedef struct { trb_t *trb; u32 n, idx, cycle; } ring_t;

/* TRB types and fields (xHCI 6.4) */
#define TRB_TYPE(t)     ((u32)(t) << 10)
#define T_NORMAL        1
#define T_SETUP         2
#define T_DATA          3
#define T_STATUS        4
#define T_ISOCH         5
#define T_LINK          6
#define T_ENABLE_SLOT   9
#define T_DISABLE_SLOT  10
#define T_ADDRESS_DEV   11
#define T_CONFIG_EP     12
#define T_EVAL_CTX      13
#define T_NOOP_CMD      23
#define E_TRANSFER      32
#define E_CMD_DONE      33
#define E_PORT_CHANGE   34
#define TRB_CYCLE       (1u << 0)
#define TRB_TC          (1u << 1)          /* link: toggle cycle */
#define TRB_ISP         (1u << 2)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_DIR_IN      (1u << 16)
#define TRB_SIA         (1u << 31)         /* isoch: start as soon as possible */
#define CC_SUCCESS      1
#define CC_SHORT        13

/* registers */
#define CAPLENGTH   0x00
#define HCSPARAMS1  0x04
#define HCSPARAMS2  0x08
#define HCCPARAMS   0x10
#define DBOFF       0x14
#define RTSOFF      0x18
#define USBCMD      0x00
#define USBSTS      0x04
#define CRCR        0x18
#define DCBAAP      0x30
#define CONFIG      0x38
#define PORTSC(n)   (0x400 + 0x10 * (n))
#define CMD_RS      1u
#define CMD_HCRST   2u
#define STS_HCH     1u
#define STS_CNR     (1u << 11)
#define PS_CCS      (1u << 0)
#define PS_PED      (1u << 1)
#define PS_PR       (1u << 4)
#define PS_PP       (1u << 9)
#define PS_SPEED(x) (((x) >> 10) & 15)
#define PS_CSC      (1u << 17)
#define PS_PRC      (1u << 21)
#define PS_KEEP     0x0e00c3e0u            /* never write back RW1C bits or PED */
#define SPEED_FULL  1
#define SPEED_LOW   2
#define SPEED_HIGH  3
#define SPEED_SUPER 4

#define MAX_DEV     16
#define RING_N      256

#define MAX_EP 8
#define IN_BUF 4096

typedef struct {
    int open, dci, in, type, mps;          /* type: 2 bulk, 3 interrupt (USB attributes) */
    ring_t ring;
    u8 *buf;                               /* IN: receive buffer; OUT: bounce buffer */
    usb_in_cb cb;
    void *arg;
    volatile int done, cc, residue;
    /* isochronous OUT: a ring of packets the class driver keeps filled */
    int iso, iso_slot, iso_n;              /* bytes per packet buffer, packets in the ring */
    u8 *iso_buf;
    volatile u32 iso_queued, iso_done;     /* packets pushed / completed (free-running) */
    usb_iso_fill fill;
    u32 iso_errs;
} uep_t;

typedef struct { u8 addr, attr, iface; u16 mps; u8 ival; } epdesc_t;

struct udev {
    int used, slot, port, speed;           /* port: the root port the device's tree hangs from */
    u32 route;                             /* route string: a hub port number per tier below the root */
    int depth;                             /* 0: on a root port */
    struct udev *parent;                   /* the hub it is plugged into (NULL: root port) */
    int pport;                             /* the port on that hub */
    int tt_slot, tt_port;                  /* full/low speed behind a high-speed hub: its transaction translator */
    int hub_ports;                         /* a hub: number of downstream ports */
    u32 hub_present;                       /* bit n: a device is attached to hub port n+1 */
    int hub_ival;                          /* hubs: next status poll */
    u8 *out, *in;                          /* device context, input context */
    ring_t ep0;
    u8 *buf;                               /* DMA page for control data */
    u16 vid, pid, mps0;
    u8 iproduct;
    u8 *cfgdesc;                           /* the whole configuration descriptor, for class drivers */
    int cfglen;
    u8 dclass, iclass, isub, iproto;
    char what[48];
    int max_dci;
    uep_t ep[MAX_EP];
    epdesc_t eps[16];
    int neps;
    int kbd;                               /* boot keyboard */
    int mouse;                             /* a pointer: its report layout */
    hidmouse_t hm;
    u8 prev[8];
    volatile int ctl_done, ctl_cc;
};

static struct {
    pci_dev_t *pci;
    volatile u8 *cap, *op, *rt;
    volatile u32 *db;
    int ports, slots, csz;                 /* context size: 32 or 64 */
    u64 *dcbaa;
    ring_t cmd;
    trb_t *evt;
    u32 evt_idx, evt_cycle;
    volatile int cmd_done;
    u64 cmd_trb;
    u32 cmd_cc, cmd_slot;
    udev_t dev[MAX_DEV];
    char status[96];
    int active;
    u32 port_change;
    /* keys from keyboards, collected in the event loop */
    event_t keys[32];
    int nkeys;
    u16 rep_scan; c16 rep_ch; u64 rep_next; int rep_on;
} x;

/* the lock: everything runs on the boot core */
#define LOCK()   u64 lock_fl_ = irq_save()
#define UNLOCK() irq_restore(lock_fl_)

static u32 rd(volatile u8 *b, u32 r) { return *(volatile u32 *)(b + r); }
static void wr(volatile u8 *b, u32 r, u32 v) { *(volatile u32 *)(b + r) = v; }
static void wr64(volatile u8 *b, u32 r, u64 v) { wr(b, r, (u32)v); wr(b, r + 4, (u32)(v >> 32)); }
static u64 phys(const void *p) { return (u64)(usize)p; }     /* identity-mapped DMA memory */

static int wait32(volatile u8 *b, u32 r, u32 mask, u32 want, u32 ms) {
    u64 end = k_now_us() + (u64)ms * 1000;
    while ((rd(b, r) & mask) != want) if (k_now_us() > end) return 0;
    return 1;
}

/* ---- rings ----------------------------------------------------------------------- */
static int ring_init(ring_t *r, u32 n) {
    r->trb = hal_dma_alloc(n * sizeof(trb_t));
    if (!r->trb) return 0;
    r->n = n; r->idx = 0; r->cycle = 1;
    r->trb[n - 1].ptr = phys(r->trb);                                 /* link back to the start */
    r->trb[n - 1].flags = TRB_TYPE(T_LINK) | TRB_TC;
    return 1;
}

/* a fresh (or reused, wiped) transfer ring */
static int ring_reset(ring_t *r) {
    if (!r->trb) return ring_init(r, RING_N);
    memset(r->trb, 0, r->n * sizeof(trb_t));
    r->idx = 0; r->cycle = 1;
    r->trb[r->n - 1].ptr = phys(r->trb);
    r->trb[r->n - 1].flags = TRB_TYPE(T_LINK) | TRB_TC;
    return 1;
}

static u64 ring_push(ring_t *r, u64 ptr, u32 status, u32 flags) {
    trb_t *t = &r->trb[r->idx];
    t->ptr = ptr;
    t->status = status;
    __asm__ volatile("" ::: "memory");
    t->flags = (flags & ~TRB_CYCLE) | r->cycle;                       /* the cycle bit hands it over */
    u64 a = phys(t);
    if (++r->idx == r->n - 1) {                                       /* reached the link TRB */
        r->trb[r->n - 1].flags = TRB_TYPE(T_LINK) | TRB_TC | r->cycle;
        r->idx = 0;
        r->cycle ^= 1;
    }
    return a;
}

/* ---- events ------------------------------------------------------------------------ */
static udev_t *dev_by_slot(u32 slot) {
    for (int i = 0; i < MAX_DEV; i++) if (x.dev[i].used && x.dev[i].slot == (int)slot) return &x.dev[i];
    return NULL;
}

static void ep_queue(udev_t *d, uep_t *e) {
    ring_push(&e->ring, phys(e->buf), e->type == 3 ? (u32)e->mps : IN_BUF, TRB_TYPE(T_NORMAL) | TRB_IOC | TRB_ISP);
    x.db[d->slot] = (u32)e->dci;
}

static uep_t *ep_by_dci(udev_t *d, u32 dci) {
    for (int i = 0; i < MAX_EP; i++) if (d->ep[i].open && d->ep[i].dci == (int)dci) return &d->ep[i];
    return NULL;
}

/* Drain the event ring.  Interrupts must be off (LOCK). */
static void events_locked(void) {
    int any = 0;
    for (;;) {
        trb_t *e = &x.evt[x.evt_idx];
        u32 fl = e->flags;
        if ((fl & 1) != x.evt_cycle) break;
        u32 type = (fl >> 10) & 63, cc = e->status >> 24, slot = fl >> 24;
        if (type == E_CMD_DONE) {
            if (e->ptr == x.cmd_trb) { x.cmd_cc = cc; x.cmd_slot = slot; x.cmd_done = 1; }
        } else if (type == E_TRANSFER) {
            udev_t *d = dev_by_slot(slot);
            u32 dci = (fl >> 16) & 31;
            uep_t *ep = d ? ep_by_dci(d, dci) : NULL;
            if (d && dci == 1) { d->ctl_cc = (int)cc; d->ctl_done = 1; }
            else if (ep && ep->in) {
                int len = (ep->type == 3 ? ep->mps : IN_BUF) - (int)(e->status & 0xffffff);
                if ((cc == CC_SUCCESS || cc == CC_SHORT) && ep->cb && len > 0) ep->cb(ep->arg, ep->buf, len);
                if (cc == CC_SUCCESS || cc == CC_SHORT) ep_queue(d, ep);
                else klog("usb: port %d endpoint %u stopped (cc %u)", d->port, dci, cc);
            } else if (ep && ep->iso) {
                ep->iso_done++;
                if (cc != CC_SUCCESS && cc != CC_SHORT) ep->iso_errs++;      /* missed service, underrun: the next packet starts afresh */
            } else if (ep) { ep->cc = (int)cc; ep->residue = (int)(e->status & 0xffffff); ep->done = 1; }
        } else if (type == E_PORT_CHANGE) {
            u32 port = (u32)(e->ptr >> 24) & 0xff;
            if (port >= 1 && port <= 32) x.port_change |= 1u << (port - 1);
        }
        if (++x.evt_idx == RING_N) { x.evt_idx = 0; x.evt_cycle ^= 1; }
        any = 1;
    }
    if (any) wr64(x.rt, 0x38, phys(&x.evt[x.evt_idx]) | 8);               /* ERDP, clear busy */
}

void usb_poll(void) {
    if (!x.evt) return;
    LOCK();
    events_locked();
    UNLOCK();
}

/* wait for a flag set by the event loop; other threads run meanwhile */
static int wait_flag(volatile int *flag, u32 ms) {
    u64 end = k_now_us() + (u64)ms * 1000;
    while (!*flag) {
        usb_poll();
        if (*flag) break;
        if (k_now_us() > end) return 0;
        thread_yield();
    }
    return 1;
}

static int command(u64 ptr, u32 status, u32 flags, u32 *slot) {
    LOCK();
    x.cmd_done = 0;
    x.cmd_trb = ring_push(&x.cmd, ptr, status, flags);
    x.db[0] = 0;
    UNLOCK();
    if (!wait_flag(&x.cmd_done, 500)) return -1;
    if (slot) *slot = x.cmd_slot;
    return (int)x.cmd_cc;
}

/* ---- contexts ------------------------------------------------------------------------ */
static u32 *ictx(udev_t *d, int i) { return (u32 *)(d->in + (usize)i * x.csz); }    /* 0 = control, 1 = slot, 1+dci = endpoint */

/* slot context dwords 0-2 for this device (route, speed, entries, root port, hub, TT) */
static void slot_ctx(udev_t *d, int entries) {
    u32 *c = ictx(d, 1);
    c[0] = (d->route & 0xfffff) | ((u32)d->speed << 20) | ((u32)(d->hub_ports ? 1 : 0) << 26) | ((u32)entries << 27);
    c[1] = ((u32)d->port << 16) | ((u32)d->hub_ports << 24);
    c[2] = (u32)d->tt_slot | ((u32)d->tt_port << 8);
}

static void ep_ctx(u32 *c, int type, int mps, int interval, u64 ring, int avg) {
    c[0] = (u32)interval << 16;
    c[1] = (3u << 1) | ((u32)type << 3) | ((u32)mps << 16);              /* CErr 3 */
    c[2] = (u32)ring | 1;                                                 /* DCS */
    c[3] = (u32)(ring >> 32);
    c[4] = (u32)avg | (type == 7 ? (u32)mps << 16 : 0);
}

/* ---- control transfers ------------------------------------------------------------------ */
static int control(udev_t *d, u8 rtype, u8 req, u16 val, u16 idx, void *data, u16 len) {
    int in = rtype & 0x80;
    if (len > 4096) return -1;
    if (!in && len) memcpy(d->buf, data, len);
    u64 setup = rtype | (u64)req << 8 | (u64)val << 16 | (u64)idx << 32 | (u64)len << 48;
    u32 trt = len ? (in ? 3u : 2u) << 16 : 0;
    LOCK();
    ring_push(&d->ep0, setup, 8, TRB_TYPE(T_SETUP) | TRB_IDT | trt);
    if (len) ring_push(&d->ep0, phys(d->buf), len, TRB_TYPE(T_DATA) | (in ? TRB_DIR_IN : 0));
    ring_push(&d->ep0, 0, 0, TRB_TYPE(T_STATUS) | TRB_IOC | (len && in ? 0 : TRB_DIR_IN));
    d->ctl_done = 0;
    x.db[d->slot] = 1;
    UNLOCK();
    if (!wait_flag(&d->ctl_done, 1000)) return -1;
    if (d->ctl_cc != CC_SUCCESS && d->ctl_cc != CC_SHORT) return -d->ctl_cc;
    if (in && len) memcpy(data, d->buf, len);
    return 0;
}

int usb_control(udev_t *d, u8 rtype, u8 req, u16 val, u16 idx, void *data, u16 len) {
    return d && d->used ? control(d, rtype, req, val, idx, data, len) : -1;
}

/* ---- endpoints ------------------------------------------------------------------------------------ */
static int ep_open(udev_t *d, u8 addr, usb_in_cb cb, void *arg) {
    epdesc_t *de = NULL;
    for (int i = 0; i < d->neps; i++) if (d->eps[i].addr == addr) de = &d->eps[i];
    if (!de || (de->attr & 3) == 0 || (de->attr & 3) == 1) return -1;      /* bulk or interrupt only */
    uep_t *e = NULL;
    for (int i = 0; i < MAX_EP; i++) if (!d->ep[i].open) { e = &d->ep[i]; break; }
    if (!e) return -1;
    ring_t keep = e->ring;
    u8 *buf = e->buf;
    memset(e, 0, sizeof *e);
    e->ring = keep;
    e->buf = buf ? buf : hal_dma_alloc(IN_BUF);
    if (!e->buf || !ring_reset(&e->ring)) return -1;
    e->in = (addr & 0x80) != 0;
    e->type = de->attr & 3;
    e->mps = de->mps & 0x7ff;
    e->dci = (addr & 15) * 2 + (e->in ? 1 : 0);
    e->cb = cb;
    e->arg = arg;
    int ival = 0;
    if (e->type == 3) {
        if (d->speed == SPEED_HIGH || d->speed == SPEED_SUPER) ival = MAX(de->ival, 1) - 1;
        else { ival = 3; while ((1 << ival) < de->ival * 8 && ival < 10) ival++; }   /* frames -> 2^n * 125 us */
    }
    int xtype = e->type == 2 ? (e->in ? 6 : 2) : (e->in ? 7 : 3);           /* xHCI endpoint types */
    if (e->dci > d->max_dci) d->max_dci = e->dci;
    memset(d->in, 0, 4096);
    ictx(d, 0)[1] = 1u | (1u << e->dci);
    slot_ctx(d, d->max_dci);
    ep_ctx(ictx(d, 1 + e->dci), xtype, e->mps, ival, phys(e->ring.trb), e->type == 3 ? e->mps : 1024);
    int cc = command(phys(d->in), 0, TRB_TYPE(T_CONFIG_EP) | ((u32)d->slot << 24), NULL);
    if (cc != CC_SUCCESS) { klog("usb: port %d: endpoint %02x not configured (cc %d)", d->port, addr, cc); return -1; }
    LOCK();
    e->open = 1;
    if (e->in) ep_queue(d, e);
    UNLOCK();
    return 0;
}

int usb_open_in(udev_t *d, u8 addr, usb_in_cb cb, void *arg) { return d && (addr & 0x80) ? ep_open(d, addr, cb, arg) : -1; }
int usb_open_out(udev_t *d, u8 addr) { return d && !(addr & 0x80) ? ep_open(d, addr, NULL, NULL) : -1; }

int usb_bulk_out(udev_t *d, u8 addr, const void *data, int len) {
    uep_t *e = NULL;
    for (int i = 0; i < MAX_EP; i++) if (d->ep[i].open && d->ep[i].dci == (addr & 15) * 2) e = &d->ep[i];
    if (!e || len > IN_BUF) return -1;
    memcpy(e->buf, data, (usize)len);
    LOCK();
    e->done = 0;
    ring_push(&e->ring, phys(e->buf), (u32)len, TRB_TYPE(T_NORMAL) | TRB_IOC);
    x.db[d->slot] = (u32)e->dci;
    UNLOCK();
    if (!wait_flag(&e->done, 1000)) return -1;
    return e->cc == CC_SUCCESS ? 0 : -e->cc;
}

int usb_find_ep(udev_t *d, int iface, int type, int in) {
    for (int i = 0; i < d->neps; i++)
        if (d->eps[i].iface == iface && (d->eps[i].attr & 3) == type && ((d->eps[i].addr & 0x80) != 0) == (in != 0)) return d->eps[i].addr;
    return 0;
}

/* ---- interfaces, strings, the configuration ---------------------------------------------- */
int usb_set_interface(udev_t *d, int iface, int alt) { return usb_control(d, 0x01, 11, (u16)alt, (u16)iface, NULL, 0); }   /* SET_INTERFACE */

const u8 *usb_config(udev_t *d, int *len) { *len = d->cfglen; return d->cfgdesc; }

int usb_string(udev_t *d, int index, char *out, int cap) {
    u8 b[128];
    out[0] = 0;
    if (!index || control(d, 0x80, 6, (u16)(0x0300 | index), 0x0409, b, 2) || b[0] < 2) return -1;
    int n = MIN(b[0], (int)sizeof b);
    if (control(d, 0x80, 6, (u16)(0x0300 | index), 0x0409, b, (u16)n)) return -1;
    int o = 0;
    for (int i = 2; i + 1 < n && o < cap - 1; i += 2) out[o++] = b[i + 1] || b[i] > 126 || b[i] < 32 ? '?' : (char)b[i];   /* UTF-16 -> ASCII */
    out[o] = 0;
    return o;
}
int usb_product_string(udev_t *d, char *out, int cap) { return usb_string(d, d->iproduct, out, cap); }
int usb_speed(udev_t *d) { return d->speed; }

/* ---- isochronous OUT (USB audio) -----------------------------------------------------------
 * The class driver gives the endpoint's packet size and interval (from the alternate
 * setting it chose) and a fill callback; usb_iso_pump() keeps 'ahead' packets queued,
 * each an Isoch TRB that starts as soon as possible after the one before it.  Called
 * from the sound thread every few milliseconds; completions are counted by the event loop. */
int usb_iso_open(udev_t *d, u8 addr, int mps, int binterval, usb_iso_fill fill, void *arg) {
    if (!d || !d->used || (addr & 0x80)) return -1;
    uep_t *e = NULL;
    for (int i = 0; i < MAX_EP; i++) if (d->ep[i].open && d->ep[i].dci == (addr & 15) * 2) e = &d->ep[i];   /* re-open after SET_INTERFACE */
    if (!e) for (int i = 0; i < MAX_EP; i++) if (!d->ep[i].open) { e = &d->ep[i]; break; }
    if (!e) return -1;
    ring_t keep = e->ring;
    u8 *buf = e->buf, *ib = e->iso_buf;
    memset(e, 0, sizeof *e);
    e->ring = keep; e->buf = buf;
    if (!ring_reset(&e->ring)) return -1;
    int base = mps & 0x7ff, extra = (mps >> 11) & 3;                   /* high speed: extra transactions per microframe */
    e->iso = 1;
    e->in = 0;
    e->type = 1;
    e->mps = base;
    e->dci = (addr & 15) * 2;
    e->iso_n = 64;
    e->iso_slot = base * (extra + 1);
    e->iso_buf = ib ? ib : hal_dma_alloc(64 * 3072);                   /* up to 3 x 1024 bytes a packet */
    if (!e->iso_buf) return -1;
    e->fill = fill;
    e->arg = arg;
    /* the interval as 2^n x 125 us: full speed counts frames (1 ms), high speed microframes */
    int bi = CLAMP(binterval, 1, 16);
    int ival = d->speed == SPEED_FULL || d->speed == SPEED_LOW ? bi - 1 + 3 : bi - 1;
    if (e->dci > d->max_dci) d->max_dci = e->dci;
    memset(d->in, 0, 4096);
    ictx(d, 0)[0] = 1u << e->dci;                                      /* drop it first (re-open), then add */
    ictx(d, 0)[1] = 1u | (1u << e->dci);
    slot_ctx(d, d->max_dci);
    u32 *c = ictx(d, 1 + e->dci);
    c[0] = (u32)ival << 16 | (u32)(e->iso_slot >> 16) << 24;           /* Max ESIT payload (hi) */
    c[1] = (1u << 3) | ((u32)base << 16) | ((u32)extra << 8);          /* type 1: isoch OUT, CErr 0, MaxBurst */
    c[2] = (u32)phys(e->ring.trb) | 1;
    c[3] = (u32)(phys(e->ring.trb) >> 32);
    c[4] = (u32)e->iso_slot | ((u32)e->iso_slot << 16);                /* average TRB length, Max ESIT payload */
    int cc = command(phys(d->in), 0, TRB_TYPE(T_CONFIG_EP) | ((u32)d->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        ictx(d, 0)[0] = 0;                                             /* nothing to drop the first time */
        cc = command(phys(d->in), 0, TRB_TYPE(T_CONFIG_EP) | ((u32)d->slot << 24), NULL);
    }
    if (cc != CC_SUCCESS) { klog("usb: port %d: isochronous endpoint %02x not configured (cc %d)", d->port, addr, cc); return -1; }
    e->open = 1;
    return 0;
}

/* queue packets until 'ahead' are outstanding; returns how many are in flight */
int usb_iso_pump(udev_t *d, u8 addr, int ahead) {
    if (!d || !d->used) return -1;
    usb_poll();                                                        /* count completions */
    uep_t *e = NULL;
    for (int i = 0; i < MAX_EP; i++) if (d->ep[i].open && d->ep[i].iso && d->ep[i].dci == (addr & 15) * 2) e = &d->ep[i];
    if (!e) return -1;
    ahead = MIN(ahead, e->iso_n - 2);
    int pushed = 0;
    LOCK();
    while ((int)(e->iso_queued - e->iso_done) < ahead) {
        u8 *b = e->iso_buf + (usize)(e->iso_queued % (u32)e->iso_n) * 3072;
        int n = e->fill(e->arg, b, e->iso_slot);
        if (n < 0) break;
        ring_push(&e->ring, phys(b), (u32)n, TRB_TYPE(T_ISOCH) | TRB_IOC | TRB_SIA);
        e->iso_queued++;
        pushed++;
    }
    if (pushed) x.db[d->slot] = (u32)e->dci;
    int inflight = (int)(e->iso_queued - e->iso_done);
    UNLOCK();
    return inflight;
}

u32 usb_iso_errors(udev_t *d, u8 addr) {
    for (int i = 0; i < MAX_EP; i++) if (d->ep[i].open && d->ep[i].iso && d->ep[i].dci == (addr & 15) * 2) return d->ep[i].iso_errs;
    return 0;
}

u16 usb_vid(udev_t *d) { return d->vid; }
u16 usb_pid(udev_t *d) { return d->pid; }
const char *usb_name(udev_t *d) { return d->what; }
void usb_set_name(udev_t *d, const char *name) { strlcpy(d->what, name, sizeof d->what); }

/* ---- keyboards (HID boot protocol) ------------------------------------------------------------- */
static void key_of(u8 u, int shift, int ctrl, u16 *scan, c16 *ch) {
    static const char lo[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    static const char hi[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    static const char p_lo[] = "-=[]\\#;'`,./", p_hi[] = "_+{}|~:\"~<>?";
    *scan = 0; *ch = 0;
    if (u >= 4 && u <= 39) { *ch = (c16)(shift ? hi[u - 4] : lo[u - 4]); if (ctrl && u <= 29) *ch = (c16)(lo[u - 4] & 0x1f); }
    else if (u >= 45 && u <= 56) *ch = (c16)(shift ? p_hi[u - 45] : p_lo[u - 45]);
    else switch (u) {
        case 40: case 88: *ch = '\r'; break;
        case 41: *scan = SCAN_ESC; break;
        case 42: *ch = 8; break;
        case 43: *ch = '\t'; break;
        case 44: *ch = ' '; break;
        case 74: *scan = SCAN_HOME; break;
        case 75: *scan = SCAN_PGUP; break;
        case 76: *scan = 0x08; break;                                     /* delete */
        case 77: *scan = SCAN_END; break;
        case 78: *scan = SCAN_PGDN; break;
        case 79: *scan = SCAN_RIGHT; break;
        case 80: *scan = SCAN_LEFT; break;
        case 81: *scan = SCAN_DOWN; break;
        case 82: *scan = SCAN_UP; break;
    }
}

/* interrupt-IN callback: a boot report (modifiers, reserved, six keys) */
static void kbd_report(void *arg, const u8 *r, int len) {
    udev_t *d = arg;
    if (len < 8) return;
    int shift = (r[0] & 0x22) != 0, ctrl = (r[0] & 0x11) != 0;
    int pressed_any = 0;
    for (int i = 2; i < 8; i++) {
        u8 u = r[i];
        if (u < 4) continue;
        pressed_any = 1;
        int was = 0;
        for (int j = 2; j < 8; j++) if (d->prev[j] == u) was = 1;
        if (was) continue;
        u16 sc; c16 ch;
        key_of(u, shift, ctrl, &sc, &ch);
        if ((sc || ch) && x.nkeys < (int)ARRAY_LEN(x.keys)) {
            x.keys[x.nkeys++] = (event_t){ .type = EV_KEY, .scan = sc, .ch = ch };
            x.rep_scan = sc; x.rep_ch = ch; x.rep_on = 1; x.rep_next = k_now_ms() + 500;
        }
    }
    if (!pressed_any) x.rep_on = 0;
    memcpy(d->prev, r, 8);
}

/* ---- mice (HID report protocol, src/drivers/hidmouse.c) ---------------------------------------- */
static void mouse_report(void *arg, const u8 *r, int len) {
    udev_t *d = arg;
    mouse_report_t m;
    if (!hm_decode(&d->hm, r, len, &m)) return;
    event_t e = { .type = EV_REL, .x = m.x, .y = m.y, .dy = m.wheel, .scan = (u16)m.buttons, .from_mouse = m.abs ? 2 : 1 };
    /* movement between two polls of the event loop adds up; a button change or a wheel step stays separate */
    if (x.nkeys) {
        event_t *l = &x.keys[x.nkeys - 1];
        if (l->type == EV_REL && l->scan == e.scan && l->from_mouse == e.from_mouse && !l->dy && !e.dy) {
            if (e.from_mouse == 2) { l->x = e.x; l->y = e.y; } else { l->x += e.x; l->y += e.y; }
            return;
        }
    }
    if (x.nkeys < (int)ARRAY_LEN(x.keys)) x.keys[x.nkeys++] = e;
}

/* a HID interface that may be a pointer: its report descriptor says, else the boot protocol */
static int mouse_setup(udev_t *d, int iface, int ep, int rdesc_len, int boot_mouse) {
    static u8 rd[1024];
    int ok = 0;
    if (rdesc_len > 0 && rdesc_len <= (int)sizeof rd && !control(d, 0x81, 6, 0x2200, (u16)iface, rd, (u16)rdesc_len))
        ok = hm_parse(&d->hm, rd, rdesc_len);
    if (!ok && boot_mouse) {
        hm_boot(&d->hm);
        control(d, 0x21, 0x0b, 0, (u16)iface, NULL, 0);                /* SET_PROTOCOL boot */
        ok = 1;
    }
    if (!ok) return 0;
    control(d, 0x21, 0x0a, 0, (u16)iface, NULL, 0);                    /* SET_IDLE 0 (may stall: fine) */
    if (ep_open(d, (u8)ep, mouse_report, d)) return 0;
    d->mouse = 1;
    klog("usb: port %d: %s ready (%s, %d buttons%s)", d->port, d->hm.x.rel || d->hm.boot ? "mouse" : "absolute pointer",
         d->hm.boot ? "boot protocol" : "report descriptor", d->hm.nbuttons, d->hm.wheel.present ? ", wheel" : "");
    return 1;
}

/* ---- enumeration ---------------------------------------------------------------------------- */
static const struct { u16 vid, pid; const char *name; } known[] = {
    { 0x8087, 0x0a2b, "Intel Wireless 8260 Bluetooth" },
    { 0x0424, 0x2807, "Microchip USB hub (USB-C dock?)" },
    { 0x8087, 0x0aa7, "Intel Wireless 3168 Bluetooth" },
};

static const char *class_name(u8 c) {
    switch (c) {
    case 0x01: return "audio"; case 0x02: return "communications"; case 0x03: return "HID";
    case 0x07: return "printer"; case 0x08: return "storage"; case 0x09: return "hub";
    case 0x0a: return "CDC data"; case 0x0b: return "smart card"; case 0x0e: return "video";
    case 0xe0: return "wireless (Bluetooth)"; case 0xef: return "miscellaneous"; case 0xff: return "vendor";
    case 0x11: return "USB-C dock (its monitor: Settings -> External display)";
    }
    return "device";
}

static void port_reset(int p) {
    u32 v = rd(x.op, PORTSC(p));
    if (!(v & PS_PP)) { wr(x.op, PORTSC(p), (v & PS_KEEP) | PS_PP); hal_delay_us(20000); v = rd(x.op, PORTSC(p)); }
    if (v & PS_PED) return;                                           /* USB 3 ports enable themselves */
    wr(x.op, PORTSC(p), (v & PS_KEEP) | PS_PR);
    wait32(x.op, PORTSC(p), PS_PRC, PS_PRC, 500);
    wr(x.op, PORTSC(p), (rd(x.op, PORTSC(p)) & PS_KEEP) | PS_PRC);     /* acknowledge */
    hal_delay_us(10000);                                               /* reset recovery */
}

static void free_dev(udev_t *d) {
    uaudio_detach(d);                                                  /* class drivers let go first */
    if (d->slot) command(0, 0, TRB_TYPE(T_DISABLE_SLOT) | ((u32)d->slot << 24), NULL);
    if (d->slot >= 0 && d->slot <= x.slots) x.dcbaa[d->slot] = 0;
    d->used = 0;                                                       /* its DMA pages are kept for reuse */
}

static void hub_setup(udev_t *d);

/* Address and configure a device that has just been reset and enabled:
 * on root port root (1-based) at the given speed, or behind hub parent's port. */
static void enumerate(int root, int speed, udev_t *parent, int pport) {
    udev_t *d = NULL;
    for (int i = 0; i < MAX_DEV; i++) if (!x.dev[i].used) { d = &x.dev[i]; break; }
    if (!d) { klog("usb: too many devices"); return; }
    /* keep a slot's DMA pages and rings across re-attaches */
    udev_t keep = *d;
    memset(d, 0, sizeof *d);
    d->out = keep.out ? keep.out : hal_dma_alloc(4096);
    d->in = keep.in ? keep.in : hal_dma_alloc(4096);
    d->buf = keep.buf ? keep.buf : hal_dma_alloc(4096);
    d->ep0 = keep.ep0;
    for (int i = 0; i < MAX_EP; i++) { d->ep[i].ring = keep.ep[i].ring; d->ep[i].buf = keep.ep[i].buf; d->ep[i].iso_buf = keep.ep[i].iso_buf; }
    if (keep.cfgdesc) kfree(keep.cfgdesc);
    if (!d->out || !d->in || !d->buf || !ring_reset(&d->ep0)) return;
    memset(d->out, 0, 4096);
    d->port = root;
    d->speed = speed;
    d->parent = parent;
    d->pport = pport;
    if (parent) {
        d->depth = parent->depth + 1;
        d->route = parent->route | ((u32)MIN(pport, 15) << (4 * parent->depth));
        if (parent->speed == SPEED_HIGH && (speed == SPEED_FULL || speed == SPEED_LOW)) { d->tt_slot = parent->slot; d->tt_port = pport; }
        else { d->tt_slot = parent->tt_slot; d->tt_port = parent->tt_port; }
    }
    u32 slot = 0;
    int cc = command(0, 0, TRB_TYPE(T_ENABLE_SLOT), &slot);
    if (cc != CC_SUCCESS || !slot || (int)slot > x.slots) { klog("usb: port %d: no slot (cc %d)", root, cc); return; }
    d->slot = (int)slot;
    d->used = 1;
    x.dcbaa[slot] = phys(d->out);
    d->mps0 = d->speed == SPEED_SUPER ? 512 : d->speed == SPEED_HIGH ? 64 : 8;
    memset(d->in, 0, 4096);
    ictx(d, 0)[1] = 3;                                                 /* add slot + EP0 */
    slot_ctx(d, 1);
    ep_ctx(ictx(d, 2), 4, d->mps0, 0, phys(d->ep0.trb), 8);
    cc = command(phys(d->in), 0, TRB_TYPE(T_ADDRESS_DEV) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) { klog("usb: port %d: address device failed (cc %d)", d->port, cc); free_dev(d); return; }
    hal_delay_us(2000);
    u8 desc[18];
    if (control(d, 0x80, 6, 0x0100, 0, desc, 8)) { klog("usb: port %d: no device descriptor", d->port); free_dev(d); return; }
    if (desc[7] && desc[7] != d->mps0 && d->speed != SPEED_SUPER) {    /* full speed: the real EP0 packet size */
        d->mps0 = desc[7];
        memset(d->in, 0, 4096);
        ictx(d, 0)[1] = 2;
        ep_ctx(ictx(d, 2), 4, d->mps0, 0, phys(d->ep0.trb), 8);
        ictx(d, 2)[2] = (u32)phys(&d->ep0.trb[d->ep0.idx]) | d->ep0.cycle;
        command(phys(d->in), 0, TRB_TYPE(T_EVAL_CTX) | (slot << 24), NULL);
    }
    if (control(d, 0x80, 6, 0x0100, 0, desc, 18)) { free_dev(d); return; }
    d->vid = (u16)(desc[8] | desc[9] << 8);
    d->pid = (u16)(desc[10] | desc[11] << 8);
    d->dclass = desc[4];
    d->iproduct = desc[15];
    /* the first configuration: interfaces and endpoints */
    static u8 cfg[1024];
    if (control(d, 0x80, 6, 0x0200, 0, cfg, 9)) { free_dev(d); return; }
    u16 total = (u16)MIN(cfg[2] | cfg[3] << 8, (int)sizeof cfg);
    if (control(d, 0x80, 6, 0x0200, 0, cfg, total)) { free_dev(d); return; }
    if (d->cfgdesc) kfree(d->cfgdesc);
    d->cfgdesc = kalloc(total);
    memcpy(d->cfgdesc, cfg, total);
    d->cfglen = total;
    int kbd_if = -1, kbd_ep = 0, cur_if = -1, cur_alt = 0, cur_cls = 0, cur_sub = 0, cur_proto = 0;
    typedef struct { int iface, ep, rlen, boot; } hidif_t;
    hidif_t hid[4];                                                    /* other HID interfaces: mice? */
    int nhid = 0, cur_rlen = 0;
    for (int o = 0; o + 2 <= total && cfg[o] >= 2; o += cfg[o]) {
        if (cfg[o + 1] == 4 && o + 9 <= total) {                       /* interface */
            cur_if = cfg[o + 2]; cur_alt = cfg[o + 3]; cur_cls = cfg[o + 5]; cur_sub = cfg[o + 6]; cur_proto = cfg[o + 7];
            cur_rlen = 0;
            if (!d->iclass) { d->iclass = (u8)cur_cls; d->isub = (u8)cur_sub; d->iproto = (u8)cur_proto; }
        } else if (cfg[o + 1] == 0x21 && o + 9 <= total && cfg[o + 6] == 0x22) {   /* HID: the report descriptor's length */
            cur_rlen = cfg[o + 7] | cfg[o + 8] << 8;
        } else if (cfg[o + 1] == 5 && o + 7 <= total && cur_alt == 0 && d->neps < (int)ARRAY_LEN(d->eps)) {   /* endpoint */
            d->eps[d->neps++] = (epdesc_t){ cfg[o + 2], cfg[o + 3], (u8)cur_if, (u16)(cfg[o + 4] | cfg[o + 5] << 8), cfg[o + 6] };
            if (cur_cls == 3 && cur_sub == 1 && cur_proto == 1 && kbd_if < 0 && (cfg[o + 2] & 0x80) && (cfg[o + 3] & 3) == 3) {
                kbd_if = cur_if; kbd_ep = cfg[o + 2];
            } else if (cur_cls == 3 && (cfg[o + 2] & 0x80) && (cfg[o + 3] & 3) == 3 && nhid < (int)ARRAY_LEN(hid) &&
                       !(nhid && hid[nhid - 1].iface == cur_if)) {
                hid[nhid++] = (hidif_t){ cur_if, cfg[o + 2], cur_rlen, cur_sub == 1 && cur_proto == 2 };
            }
        }
    }
    const char *nm = NULL;
    for (usize i = 0; i < ARRAY_LEN(known); i++) if (known[i].vid == d->vid && known[i].pid == d->pid) nm = known[i].name;
    if (d->vid == 0x17e9) nm = "DisplayLink display adapter (closed protocol, not supported)";
    if (kbd_if >= 0 && !nm) nm = "USB keyboard";
    else if (!nm && d->iclass == 3 && d->iproto == 2) nm = "USB mouse";
    fmt(d->what, sizeof d->what, "%s", nm ? nm : class_name(d->iclass ? d->iclass : d->dclass));
    control(d, 0x00, 9, cfg[5], 0, NULL, 0);                           /* SET_CONFIGURATION */
    if (d->dclass == 9) { hub_setup(d); return; }
    klog("usb: port %d%s: %04x:%04x %s, %s speed, class %02x/%02x, %d endpoints", d->port, d->parent ? " (behind a hub)" : "", d->vid, d->pid, d->what,
         d->speed == SPEED_SUPER ? "super" : d->speed == SPEED_HIGH ? "high" : d->speed == SPEED_LOW ? "low" : "full",
         d->dclass, d->iclass, d->neps);

    if (kbd_if >= 0) {                                                 /* boot keyboard */
        control(d, 0x21, 0x0b, 0, (u16)kbd_if, NULL, 0);               /* SET_PROTOCOL boot */
        control(d, 0x21, 0x0a, 0, (u16)kbd_if, NULL, 0);               /* SET_IDLE 0 */
        memset(d->prev, 0, sizeof d->prev);
        if (!ep_open(d, (u8)kbd_ep, kbd_report, d)) { d->kbd = 1; klog("usb: port %d: keyboard ready", d->port); }
    }
    /* mice: a mouse on its own, or the pointer half of a keyboard-and-mouse receiver */
    for (int i = 0; i < nhid && !d->mouse; i++) mouse_setup(d, hid[i].iface, hid[i].ep, hid[i].rlen, hid[i].boot);
    if (d->mouse && !d->kbd && !strcmp(d->what, "HID")) strlcpy(d->what, d->hm.x.rel || d->hm.boot ? "USB mouse" : "USB pointer (tablet)", sizeof d->what);
    if (kbd_if >= 0 || d->mouse) return;
    /* USB audio: an Audio Control interface (class 1, subclass 1) somewhere in the configuration */
    if (uaudio_probe(d)) return;
    /* Bluetooth: the wireless-controller class (e0/01/01), as Linux's btusb matches it */
    if (d->dclass == 0xe0 || (d->iclass == 0xe0 && d->isub == 1 && d->iproto == 1)) bt_usb_attach(d);
}

static void attach(int p) {
    port_reset(p);
    u32 v = rd(x.op, PORTSC(p));
    if (!(v & PS_CCS) || !(v & PS_PED)) { klog("usb: port %d did not enable (%08x)", p + 1, v); return; }
    enumerate(p + 1, PS_SPEED(v), NULL, 0);
}

/* free a device and everything plugged into it */
static void detach_tree(udev_t *d) {
    for (int i = 0; i < MAX_DEV; i++) if (x.dev[i].used && x.dev[i].parent == d) detach_tree(&x.dev[i]);
    klog("usb: port %d: %s unplugged", d->port, d->what);
    free_dev(d);
}

/* ---- USB 2 hubs (USB 2.0 chapter 11) -------------------------------------------------------- */
#define HUB_PORT_POWER   8
#define HUB_PORT_RESET   4
#define HUB_C_CONNECTION 16
#define HUB_C_RESET      20

static int hub_status(udev_t *h, int port, u16 *st, u16 *chg) {
    u8 b[4];
    if (control(h, 0xa3, 0, 0, (u16)port, b, 4)) return -1;            /* GET_STATUS (port) */
    *st = (u16)(b[0] | b[1] << 8);
    *chg = (u16)(b[2] | b[3] << 8);
    return 0;
}

static void hub_setup(udev_t *h) {
    u8 hd[16];
    if (h->speed == SPEED_SUPER || control(h, 0xa0, 6, 0x2900, 0, hd, 9)) {   /* GET_DESCRIPTOR (hub) */
        klog("usb: port %d: %s hub - not supported", h->port, h->speed == SPEED_SUPER ? "USB 3" : "unreadable");
        strlcpy(h->what, "USB hub (not supported)", sizeof h->what);
        return;
    }
    h->hub_ports = MIN(hd[2], 15);
    /* tell the controller it is a hub: slot context Hub, Number of Ports (and TT think time) */
    memset(h->in, 0, 4096);
    ictx(h, 0)[1] = 1;
    slot_ctx(h, MAX(h->max_dci, 1));
    if (h->speed == SPEED_HIGH) ictx(h, 1)[2] |= (u32)((hd[3] >> 5) & 3) << 16;
    int cc = command(phys(h->in), 0, TRB_TYPE(T_EVAL_CTX) | ((u32)h->slot << 24), NULL);
    if (cc != CC_SUCCESS) cc = command(phys(h->in), 0, TRB_TYPE(T_CONFIG_EP) | ((u32)h->slot << 24), NULL);
    fmt(h->what, sizeof h->what, "USB hub, %d ports", h->hub_ports);
    klog("usb: port %d%s: %04x:%04x hub with %d ports, %s speed (context cc %d)", h->port, h->parent ? " (behind a hub)" : "",
         h->vid, h->pid, h->hub_ports, h->speed == SPEED_HIGH ? "high" : "full", cc);
    for (int i = 1; i <= h->hub_ports; i++) control(h, 0x23, 3, HUB_PORT_POWER, (u16)i, NULL, 0);   /* SET_FEATURE */
    thread_sleep_ms(MAX(hd[5] * 2, 100));                             /* power-on to power-good */
    h->hub_ival = 0;
}

static void hub_poll(udev_t *h) {
    for (int port = 1; port <= h->hub_ports; port++) {
        u16 st, chg;
        if (hub_status(h, port, &st, &chg)) return;
        if (chg & 1) control(h, 0x23, 1, HUB_C_CONNECTION, (u16)port, NULL, 0);   /* CLEAR_FEATURE */
        int present = (h->hub_present >> (port - 1)) & 1;
        if ((st & 1) && !present) {
            thread_sleep_ms(100);                                      /* debounce */
            control(h, 0x23, 3, HUB_PORT_RESET, (u16)port, NULL, 0);
            int ok = 0;
            for (int t = 0; t < 50 && !ok; t++) {
                thread_sleep_ms(10);
                if (hub_status(h, port, &st, &chg)) return;
                ok = (chg & 0x10) && (st & 2);                         /* reset done, enabled */
            }
            control(h, 0x23, 1, HUB_C_RESET, (u16)port, NULL, 0);
            if (!ok) { klog("usb: hub port %d did not enable", port); continue; }
            thread_sleep_ms(10);
            int speed = (st & 0x200) ? SPEED_LOW : (st & 0x400) ? SPEED_HIGH : SPEED_FULL;
            h->hub_present |= 1u << (port - 1);
            enumerate(h->port, speed, h, port);
        } else if (!(st & 1) && present) {
            h->hub_present &= ~(1u << (port - 1));
            for (int i = 0; i < MAX_DEV; i++)
                if (x.dev[i].used && x.dev[i].parent == h && x.dev[i].pport == port) detach_tree(&x.dev[i]);
        }
    }
}

static void scan_ports(void) {
    for (int p = 0; p < x.ports; p++) {
        u32 v = rd(x.op, PORTSC(p));
        if (v & PS_CSC) wr(x.op, PORTSC(p), (v & PS_KEEP) | PS_CSC);
        udev_t *d = NULL;
        for (int i = 0; i < MAX_DEV; i++) if (x.dev[i].used && x.dev[i].port == p + 1 && !x.dev[i].parent) d = &x.dev[i];
        if ((v & PS_CCS) && !d) attach(p);
        else if (!(v & PS_CCS) && d) detach_tree(d);
    }
}

/* ---- bring-up ----------------------------------------------------------------------------- */
static void bios_handoff(void) {
    u32 xecp = (rd(x.cap, HCCPARAMS) >> 16) & 0xffff;
    for (u32 off = xecp * 4; xecp && off; ) {
        u32 v = rd(x.cap, off);
        if ((v & 0xff) == 1) {                                         /* USB legacy support */
            wr(x.cap, off, v | (1u << 24));                            /* OS owned */
            if (!wait32(x.cap, off, 1u << 16, 0, 1000)) klog("usb: the firmware did not let go of the controller");
            u32 ctl = rd(x.cap, off + 4);
            wr(x.cap, off + 4, (ctl & ~0x0001e011u) | 0xe0000000u);   /* SMIs off, clear their status */
            return;
        }
        u32 next = (v >> 8) & 0xff;
        off = next ? off + next * 4 : 0;
    }
}

int xhci_probe(pci_dev_t *pd) {
    if (!k.native || x.active) return 0;
    x.pci = pd;
    u32 cmd = pci_read32(pd->bus, pd->dev, pd->fn, 4);
    pci_write32(pd->bus, pd->dev, pd->fn, 4, cmd | 0x6);
    u64 bar = pci_bar(pd->bus, pd->dev, pd->fn, 0);
    if (!bar) { strlcpy(x.status, "no register BAR", sizeof x.status); return 0; }
    x.cap = mm_map_mmio(bar, 0x10000);
    if (!x.cap) { strlcpy(x.status, "registers could not be mapped", sizeof x.status); return 0; }
    x.op = x.cap + (rd(x.cap, CAPLENGTH) & 0xff);
    x.rt = x.cap + (rd(x.cap, RTSOFF) & ~0x1fu);
    x.db = (volatile u32 *)(x.cap + (rd(x.cap, DBOFF) & ~3u));
    u32 hcs1 = rd(x.cap, HCSPARAMS1), hcs2 = rd(x.cap, HCSPARAMS2), hcc = rd(x.cap, HCCPARAMS);
    x.ports = (int)MIN((hcs1 >> 24) & 0xff, 32u);
    x.slots = (int)MIN(hcs1 & 0xff, (u32)MAX_DEV);
    x.csz = (hcc >> 2) & 1 ? 64 : 32;

    bios_handoff();
    if (pd->vendor == 0x8086) {                                        /* Intel: route every port to xHCI */
        pci_write32(pd->bus, pd->dev, pd->fn, 0xd8, pci_read32(pd->bus, pd->dev, pd->fn, 0xdc));
        pci_write32(pd->bus, pd->dev, pd->fn, 0xd0, pci_read32(pd->bus, pd->dev, pd->fn, 0xd4));
    }
    /* halt, reset */
    wr(x.op, USBCMD, rd(x.op, USBCMD) & ~CMD_RS);
    if (!wait32(x.op, USBSTS, STS_HCH, STS_HCH, 100)) { strlcpy(x.status, "did not halt", sizeof x.status); return 0; }
    wr(x.op, USBCMD, CMD_HCRST);
    if (!wait32(x.op, USBCMD, CMD_HCRST, 0, 1000) || !wait32(x.op, USBSTS, STS_CNR, 0, 1000)) {
        strlcpy(x.status, "reset did not finish", sizeof x.status); return 0;
    }
    wr(x.op, CONFIG, (u32)x.slots);
    /* device context array, scratchpad, command ring, event ring */
    x.dcbaa = hal_dma_alloc(4096);
    u32 spb = ((hcs2 >> 16) & 0x3e0) | ((hcs2 >> 27) & 0x1f);
    if (spb) {
        u64 *arr = hal_dma_alloc(MAX(spb * 8, 64u));
        for (u32 i = 0; i < spb; i++) arr[i] = phys(hal_dma_alloc(4096));
        x.dcbaa[0] = phys(arr);
    }
    wr64(x.op, DCBAAP, phys(x.dcbaa));
    if (!ring_init(&x.cmd, RING_N)) { strlcpy(x.status, "out of memory", sizeof x.status); return 0; }
    wr64(x.op, CRCR, phys(x.cmd.trb) | 1);
    x.evt = hal_dma_alloc(RING_N * sizeof(trb_t));
    u64 *erst = hal_dma_alloc(64);
    erst[0] = phys(x.evt);
    erst[1] = RING_N;
    x.evt_idx = 0; x.evt_cycle = 1;
    wr(x.rt, 0x28, 1);                                                 /* ERSTSZ */
    wr64(x.rt, 0x38, phys(x.evt));                                     /* ERDP */
    wr64(x.rt, 0x30, phys(erst));                                      /* ERSTBA (last) */
    wr(x.rt, 0x20, 0);                                                 /* IMAN: interrupts off, we poll */
    wr(x.op, USBCMD, CMD_RS);
    if (!wait32(x.op, USBSTS, STS_HCH, 0, 100)) { strlcpy(x.status, "did not start", sizeof x.status); return 0; }
    if (command(0, 0, TRB_TYPE(T_NOOP_CMD), NULL) != CC_SUCCESS) { strlcpy(x.status, "command ring dead", sizeof x.status); return 0; }
    x.active = 1;
    for (int p = 0; p < x.ports; p++) {                                /* power every port */
        u32 v = rd(x.op, PORTSC(p));
        if (!(v & PS_PP)) wr(x.op, PORTSC(p), (v & PS_KEEP) | PS_PP);
    }
    hal_delay_us(100000);                                              /* let devices connect */
    scan_ports();
    int n = 0;
    for (int i = 0; i < MAX_DEV; i++) n += x.dev[i].used;
    fmt(x.status, sizeof x.status, "xHCI %x.%02x, %d ports, %d device%s", rd(x.cap, CAPLENGTH) >> 24, (rd(x.cap, CAPLENGTH) >> 16) & 0xff,
        x.ports, n, n == 1 ? "" : "s");
    klog("usb: %s", x.status);
    return 1;
}

int xhci_poll(event_t *out, int max) {
    if (!x.active) return 0;
    LOCK();
    events_locked();
    int n = 0;
    while (n < max && n < x.nkeys) { out[n] = x.keys[n]; n++; }
    x.nkeys = 0;
    UNLOCK();
    if (x.port_change) {
        x.port_change = 0;
        scan_ports();
        int c = 0;
        for (int i = 0; i < MAX_DEV; i++) c += x.dev[i].used;
        fmt(x.status, sizeof x.status, "xHCI, %d ports, %d device%s", x.ports, c, c == 1 ? "" : "s");
    }
    u64 now = k_now_ms();
    static u64 next_hub;
    if (now >= next_hub) {
        next_hub = now + 250;
        for (int i = 0; i < MAX_DEV; i++) if (x.dev[i].used && x.dev[i].hub_ports) hub_poll(&x.dev[i]);
    }
    if (x.rep_on && now >= x.rep_next && n < max) {                    /* key repeat */
        out[n++] = (event_t){ .type = EV_KEY, .scan = x.rep_scan, .ch = x.rep_ch };
        x.rep_next = now + 35;
    }
    return n;
}

void xhci_status(char *buf, usize cap) { strlcpy(buf, x.status[0] ? x.status : "not started", cap); }

int xhci_devices(char lines[][96], int max) {
    int n = 0;
    for (int i = 0; i < MAX_DEV && n < max; i++) {
        udev_t *d = &x.dev[i];
        if (!d->used) continue;
        fmt(lines[n++], 96, "port %d: %s (%04x:%04x)", d->port, d->what, d->vid, d->pid);
    }
    return n;
}

#else
int xhci_probe(pci_dev_t *d) { (void)d; return 0; }
int xhci_poll(event_t *out, int max) { (void)out; (void)max; return 0; }
void xhci_status(char *buf, usize cap) { strlcpy(buf, "needs the 64-bit native kernel", cap); }
int xhci_devices(char lines[][96], int max) { (void)lines; (void)max; return 0; }
#endif
