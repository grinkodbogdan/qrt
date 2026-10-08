/* xhci.h - USB 3 host controller (xHCI) and the USB devices on its root ports. */
#pragma once
#include "../pci.h"

int  xhci_probe(pci_dev_t *d);                 /* native mode: take over the controller, enumerate */
int  xhci_poll(event_t *out, int max);         /* events, hotplug, keyboard reports -> key events */
void xhci_status(char *buf, usize cap);
int  xhci_devices(char lines[][96], int max);  /* one line per USB device, for System Monitor */
void usb_key_of(u8 usage, int shift, int ctrl, u16 *scan, c16 *ch);   /* a HID keyboard usage -> key event */
