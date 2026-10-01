/*
 * btusb.c - Bluetooth controllers on USB (class e0/01/01), as Linux's
 * btusb.c drives them: HCI commands go to the control endpoint as class
 * requests, events come from the interrupt-IN endpoint, ACL data and (for
 * Intel's bootloader) events from bulk-IN, and bulk-OUT carries ACL data and
 * the bootloader's Secure Send.  Events longer than the endpoint's packet
 * size arrive in pieces and are put back together here.
 *
 * Set-up and scanning block for seconds (the Intel firmware is ~2300
 * acknowledged commands), so they run in their own kernel thread.
 */
#include "bt.h"
#include "../usb/usb.h"

#if defined(__x86_64__)
#include "../../arch/x64/sched.h"
#include "../../kernel/vfs.h"

static struct {
    udev_t *d;
    u8 ev_in, bulk_in, bulk_out;
    u8 ibuf[300], bbuf[300];               /* reassembly: interrupt and bulk streams */
    int ilen, blen;
    int started;
} U;

/* complete HCI events (code, length, parameters) out of a byte stream */
static void reassemble(u8 *buf, int *have, const u8 *data, int len) {
    while (len > 0) {
        int room = (int)sizeof U.ibuf - *have, n = MIN(len, room);
        if (n <= 0) { *have = 0; return; }                 /* garbage: start over */
        memcpy(buf + *have, data, (usize)n);
        *have += n; data += n; len -= n;
        while (*have >= 2 && *have >= 2 + buf[1]) {
            int pl = 2 + buf[1];
            bt_rx_event(buf, pl);
            memmove(buf, buf + pl, (usize)(*have - pl));
            *have -= pl;
        }
    }
}

static void on_event(void *arg, const u8 *data, int len) { (void)arg; reassemble(U.ibuf, &U.ilen, data, len); }
static void on_bulk(void *arg, const u8 *data, int len) {
    (void)arg;
    if (bt_in_bootloader()) reassemble(U.bbuf, &U.blen, data, len);   /* Intel bootloader: events */
    /* else ACL data: no L2CAP yet */
}

static int send_cmd(const u8 *p, int len) { return usb_control(U.d, 0x20, 0, 0, 0, (void *)p, (u16)len); }
static int send_bulk(const u8 *p, int len) { return U.bulk_out ? usb_bulk_out(U.d, U.bulk_out, p, len) : -1; }
static void sleep_ms(u32 ms) { thread_sleep_ms(ms); }
static u8 *read_file(const char *path, u64 *len) {
    vnode_t *n = vfs_lookup(path);
    if (!n || n->dir) return NULL;
    *len = vfs_size(n);
    u8 *b = kalloc(*len ? *len : 1);
    vfs_read(n, 0, b, *len);
    return b;
}
static void free_file(u8 *p) { kfree(p); }

static const bt_transport_t transport = { send_cmd, send_bulk, usb_poll, sleep_ms, read_file, free_file };

static void bt_thread(void *arg) {
    (void)arg;
    U.ev_in = (u8)usb_find_ep(U.d, 0, 3, 1);
    U.bulk_in = (u8)usb_find_ep(U.d, 0, 2, 1);
    U.bulk_out = (u8)usb_find_ep(U.d, 0, 2, 0);
    if (!U.ev_in || usb_open_in(U.d, U.ev_in, on_event, NULL)) { klog("bt: no event endpoint"); thread_exit(); }
    if (U.bulk_in) usb_open_in(U.d, U.bulk_in, on_bulk, NULL);
    if (U.bulk_out) usb_open_out(U.d, U.bulk_out);
    bt_attach(&transport, usb_vid(U.d), usb_pid(U.d));
    bt_run();
}

void bt_usb_attach(udev_t *d) {
    if (U.started) return;                 /* one controller */
    U.started = 1;
    U.d = d;
    klog("bt: controller %04x:%04x on USB", usb_vid(d), usb_pid(d));
    thread_create("bluetooth", bt_thread, NULL, 0);
}

#else
void bt_usb_attach(struct udev *d) { (void)d; }
#endif
