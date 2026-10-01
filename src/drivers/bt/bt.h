/* bt.h - Bluetooth: the HCI core (hci.c) and its USB transport (btusb.c). */
#pragma once
#include "../../kernel/kernel.h"

/* ---- for the rest of QRT -------------------------------------------------------- */
typedef struct {
    u8 addr[6];
    char name[48];
    int rssi;                   /* dBm, 0 = unknown */
    u32 cod;                    /* class of device (BR/EDR) */
    int le;                     /* found by an LE scan */
    int named;
} bt_device_t;

#define BT_MAX_DEV 32
enum { BT_NONE, BT_STARTING, BT_READY, BT_SCANNING, BT_FAILED };

int  bt_state(void);
const char *bt_status(void);                  /* one line: adapter, address, firmware or the error */
int  bt_devices(bt_device_t *out, int max);   /* what the last scan found */
void bt_scan(void);                           /* start a scan (about 12 s, classic and LE) */
const char *bt_kind(u32 cod, int le);         /* "phone", "headset"... from the class of device */

/* ---- the transport a controller is reached through -------------------------------- */
typedef struct {
    int  (*send_cmd)(const u8 *pkt, int len);     /* HCI command packet (control endpoint) */
    int  (*send_bulk)(const u8 *pkt, int len);    /* bulk OUT (Intel bootloader secure send, ACL) */
    void (*poll)(void);                           /* let the transport deliver input */
    void (*sleep_ms)(u32 ms);
    u8  *(*read_file)(const char *path, u64 *len);
    void (*free_file)(u8 *p);
} bt_transport_t;

void bt_attach(const bt_transport_t *t, u16 vid, u16 pid);   /* set the controller up; blocks (run in a thread) */
void bt_run(void);                                           /* the controller's loop: scans on request; never returns */
void bt_rx_event(const u8 *evt, int len);                    /* an HCI event packet arrived (any context) */
int  bt_in_bootloader(void);                                 /* Intel: bulk IN carries events, not ACL */

struct udev;
void bt_usb_attach(struct udev *d);                          /* xhci.c: a Bluetooth controller was enumerated */
