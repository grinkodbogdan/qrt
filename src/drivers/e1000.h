/* e1000.h - Intel 8254x/82574 Ethernet (QEMU's default NIC), for testing the network stack. */
#pragma once
#include "../kernel/kernel.h"
#include "pci.h"

int  e1000_probe(pci_dev_t *d);
int  e1000_start(void);
void e1000_poll(void);
const char *e1000_status(void);
