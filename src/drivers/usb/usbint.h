/* usbint.h - inside QRT's USB stack: the device and endpoint records shared by the
 * host controller drivers (xhci.c, ehci.c) and the enumeration, hub and class-driver
 * code in xhci.c.  Class drivers use usb.h only. */
#pragma once
#include "../../kernel/kernel.h"
#include "usb.h"
#include "../hidmouse.h"
#include "../hidparse.h"

typedef struct { volatile u64 ptr; volatile u32 status, flags; } trb_t;
typedef struct { trb_t *trb; u32 n, idx, cycle; } ring_t;

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
    int nq, qnext;                         /* interrupt IN: transfers kept queued, the next buffer slice */
    /* EHCI: the endpoint's queue head and its one transfer descriptor */
    void *eqh, *eqtd;
    int elen, ebusy;
} uep_t;

typedef struct { u8 addr, attr, iface; u16 mps; u8 ival; } epdesc_t;

struct udev {
    int hc;                                /* 0: xHCI; 1: EHCI (slot is then the USB address) */
    int ehc;                               /* EHCI: which controller */
    void *eqh0, *eqtd0;                    /* EHCI: EP0's queue head, its three transfer descriptors */
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
    i2chid_t *touch;                       /* a touchscreen: its fingers (src/drivers/hidparse.c) */
    int touch_fingers;
    u8 prev[8];
    volatile int ctl_done, ctl_cc;
};

/* xhci.c, for the EHCI driver */
void usb_enumerate(int hc, int ehc, int root, int speed, udev_t *parent, int pport);
void usb_detach_tree(udev_t *d);
udev_t *usb_devices(void);                 /* the MAX_DEV device records */

/* ehci.c */
int  ehci_address(udev_t *d);              /* EP0 queue head, SET_ADDRESS; 0 or < 0 */
void ehci_set_mps0(udev_t *d);
int  ehci_control(udev_t *d, u8 rtype, u8 req, u16 val, u16 idx, void *data, u16 len);
int  ehci_ep_open(udev_t *d, uep_t *e);
int  ehci_bulk_out(udev_t *d, uep_t *e, const void *data, int len);
void ehci_free_dev(udev_t *d);
void ehci_poll_locked(void);               /* completions: IN callbacks (interrupts off) */
void ehci_ports(void);                     /* root port changes (every 250 ms) */
int  ehci_count(void);
