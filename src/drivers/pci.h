/* pci.h - PCI Express configuration access through ECAM (ACPI MCFG). */
#pragma once
#include "../kernel/kernel.h"

typedef struct {
    u8 bus, dev, fn;
    u16 vendor, device;
    u8 class_code, subclass, prog_if;
    const char *driver;          /* name of the QRT driver bound to it, if any */
} pci_dev_t;

#define PCI_MAX_DEVS 64
extern pci_dev_t pci_devs[PCI_MAX_DEVS];
extern int pci_ndevs;

int  pci_init(void);                                  /* parse MCFG, scan buses; returns device count */
int  pci_available(void);
u32  pci_read32(u8 bus, u8 dev, u8 fn, u16 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u16 off, u32 v);
u16  pci_read16(u8 bus, u8 dev, u8 fn, u16 off);
u64  pci_bar(u8 bus, u8 dev, u8 fn, int bar);         /* memory BAR address (64-bit aware) */
void pci_set_driver(u8 bus, u8 dev, u8 fn, const char *name);
int  pci_find_cap(u8 bus, u8 dev, u8 fn, u8 cap_id); /* config offset, 0 if absent */
