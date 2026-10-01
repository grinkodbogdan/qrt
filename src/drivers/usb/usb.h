/* usb.h - what USB class drivers (Bluetooth, keyboards) get from the xHCI driver.
 *
 * Endpoints are opened by address.  IN endpoints (bulk or interrupt) then
 * receive continuously: each completed transfer calls the driver's callback
 * from the event loop, with interrupts off - copy the data and return.  OUT
 * transfers and control requests are synchronous; they may be called from
 * any kernel thread, and while they wait the event loop keeps running. */
#pragma once
#include "../../kernel/kernel.h"

typedef struct udev udev_t;
typedef void (*usb_in_cb)(void *arg, const u8 *data, int len);

int  usb_control(udev_t *d, u8 rtype, u8 req, u16 val, u16 idx, void *data, u16 len);   /* 0 or < 0 */
int  usb_open_in(udev_t *d, u8 ep_addr, usb_in_cb cb, void *arg);                        /* 0 or < 0 */
int  usb_open_out(udev_t *d, u8 ep_addr);
int  usb_bulk_out(udev_t *d, u8 ep_addr, const void *data, int len);                     /* 0 or < 0 */
void usb_poll(void);                     /* run the event loop once (for drivers waiting on IN data) */
int  usb_find_ep(udev_t *d, int iface, int type, int in);  /* endpoint address (type 2 bulk, 3 interrupt), 0 if none */
u16  usb_vid(udev_t *d);
u16  usb_pid(udev_t *d);
const char *usb_name(udev_t *d);
void usb_set_name(udev_t *d, const char *name);
