/* ehci.h - USB 2 host controllers (EHCI): the internal USB devices of PCs whose ports
 * are not all on xHCI.  The devices are served by the same code as xhci.c's (usb.h). */
#pragma once
#include "../pci.h"

int  ehci_probe(pci_dev_t *p);              /* native mode: take over the controller, enumerate */
void ehci_status(pci_dev_t *p, char *buf, usize cap);
