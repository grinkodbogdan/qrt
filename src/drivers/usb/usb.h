/* usb.h - what USB class drivers (Bluetooth, keyboards, audio) get from the xHCI driver.
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
typedef int (*usb_iso_fill)(void *arg, u8 *packet, int max);   /* fill one isochronous packet: bytes, or < 0 to stop */

int  usb_control(udev_t *d, u8 rtype, u8 req, u16 val, u16 idx, void *data, u16 len);   /* 0 or < 0 */
int  usb_open_in(udev_t *d, u8 ep_addr, usb_in_cb cb, void *arg);                        /* 0 or < 0 */
int  usb_open_out(udev_t *d, u8 ep_addr);
int  usb_bulk_out(udev_t *d, u8 ep_addr, const void *data, int len);                     /* 0 or < 0 */
void usb_poll(void);                     /* run the event loop once (for drivers waiting on IN data) */
int  usb_find_ep(udev_t *d, int iface, int type, int in);  /* endpoint address (type 2 bulk, 3 interrupt), 0 if none */
int  usb_set_interface(udev_t *d, int iface, int alt);
const u8 *usb_config(udev_t *d, int *len);                     /* the whole configuration descriptor */
int  usb_string(udev_t *d, int index, char *out, int cap);     /* a string descriptor as ASCII; < 0 if none */
int  usb_product_string(udev_t *d, char *out, int cap);
int  usb_speed(udev_t *d);                                     /* 1 full, 2 low, 3 high, 4 super */
/* isochronous OUT: open with the alternate setting's packet size and interval, then
 * call usb_iso_pump() every few ms to keep 'ahead' packets queued (sound thread) */
int  usb_iso_open(udev_t *d, u8 ep_addr, int mps, int binterval, usb_iso_fill fill, void *arg);
int  usb_iso_pump(udev_t *d, u8 ep_addr, int ahead);           /* packets in flight, < 0 if gone */
u32  usb_iso_errors(udev_t *d, u8 ep_addr);
u16  usb_vid(udev_t *d);
u16  usb_pid(udev_t *d);
const char *usb_name(udev_t *d);
void usb_set_name(udev_t *d, const char *name);
