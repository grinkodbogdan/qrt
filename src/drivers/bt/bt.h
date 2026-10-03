/* bt.h - Bluetooth: the HCI core (hci.c), L2CAP and SDP (l2cap.c), A2DP audio (a2dp.c),
 * the SBC codec (sbc.c) and the USB transport (btusb.c). */
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
    int paired;                 /* a link key is stored */
} bt_device_t;

#define BT_MAX_DEV 32
enum { BT_NONE, BT_STARTING, BT_READY, BT_SCANNING, BT_FAILED };

int  bt_state(void);
const char *bt_status(void);                  /* one line: adapter, address, firmware or the error */
int  bt_devices(bt_device_t *out, int max);   /* what the last scan found, and paired devices */
void bt_scan(void);                           /* start a scan (about 12 s, classic and LE) */
const char *bt_kind(u32 cod, int le);         /* "phone", "headset"... from the class of device */

/* audio devices (A2DP sinks: headphones, speakers) */
void bt_audio_connect(const u8 addr[6]);      /* pair if needed, connect, stream (the bluetooth thread does it) */
void bt_audio_disconnect(void);
const char *bt_audio_status(void);            /* "" when idle; "Connecting to ...", "Playing on ...", or the error */
int  bt_audio_connected(const u8 addr[6]);    /* 1 while streaming to it */
void bt_forget(const u8 addr[6]);             /* drop its link key */

/* ---- the transport a controller is reached through -------------------------------- */
typedef struct {
    int  (*send_cmd)(const u8 *pkt, int len);     /* HCI command packet (control endpoint) */
    int  (*send_bulk)(const u8 *pkt, int len);    /* bulk OUT (Intel bootloader secure send, ACL) */
    void (*poll)(void);                           /* let the transport deliver input */
    void (*sleep_ms)(u32 ms);
    u8  *(*read_file)(const char *path, u64 *len);
    void (*free_file)(u8 *p);
    u64  (*now_ms)(void);
} bt_transport_t;

void bt_attach(const bt_transport_t *t, u16 vid, u16 pid);   /* set the controller up; blocks (run in a thread) */
void bt_run(void);                                           /* the controller's loop; never returns */
void bt_rx_event(const u8 *evt, int len);                    /* an HCI event packet arrived (any context) */
void bt_rx_acl(const u8 *pkt, int len);                      /* an HCI ACL data packet arrived (any context) */
int  bt_in_bootloader(void);                                 /* Intel: bulk IN carries events, not ACL */
void bt_poll_once(void);                                     /* one turn of bt_run's loop (tests) */

struct udev;
void bt_usb_attach(struct udev *d);                          /* xhci.c: a Bluetooth controller was enumerated */

/* ---- inside the stack ------------------------------------------------------------------ */
/* hci.c */
int  hci_send_acl(u16 handle, const u8 *l2cap, int len);     /* one L2CAP frame (fragmented to the controller's MTU); 0 or < 0; bluetooth thread only */
int  hci_acl_room(void);                                     /* ACL packets the controller can take now */
void hci_disconnect(u16 handle);
u64  bt_now_ms(void);
void bt_set_audio_status(const char *f, const char *name);  /* f has one %s: the device's name */

/* l2cap.c */
typedef struct l2cap_ch l2cap_ch_t;
typedef struct {
    void (*open)(l2cap_ch_t *ch);
    void (*data)(l2cap_ch_t *ch, const u8 *p, int len);
    void (*closed)(l2cap_ch_t *ch);
} l2cap_ops_t;
void l2cap_rx(u16 handle, const u8 *frame, int len);         /* a whole L2CAP frame from hci.c */
void l2cap_link_up(u16 handle);
void l2cap_link_down(u16 handle);
l2cap_ch_t *l2cap_connect(u16 handle, u16 psm, const l2cap_ops_t *ops, void *user);
void l2cap_listen(u16 psm, const l2cap_ops_t *ops);          /* accept incoming channels to psm */
int  l2cap_send(l2cap_ch_t *ch, const u8 *p, int len);
void l2cap_close(l2cap_ch_t *ch);
int  l2cap_mtu(l2cap_ch_t *ch);                              /* the largest payload the other side takes */
void *l2cap_user(l2cap_ch_t *ch);
u16  l2cap_handle(l2cap_ch_t *ch);

/* a2dp.c */
void a2dp_init(void);
void a2dp_link_ready(u16 handle, const u8 addr[6], const char *name, int initiator);   /* connected and encrypted */
void a2dp_link_down(u16 handle);
void a2dp_poll(void);                                        /* the bluetooth thread: timeouts */
