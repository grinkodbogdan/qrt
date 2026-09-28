/*
 * pci.c - PCI Express configuration space via ECAM.
 *
 * The ACPI MCFG table gives the physical base of the memory-mapped
 * configuration space; every function's 4 KiB of registers sits at
 * base + (bus << 20 | dev << 15 | fn << 12).  This works identically with
 * the firmware still resident or after it has been shut down.
 */
#include "pci.h"

pci_dev_t pci_devs[PCI_MAX_DEVS];
int pci_ndevs;

static volatile u8 *ecam;
static u8 bus_first, bus_last;


static volatile u32 *cfg(u8 bus, u8 dev, u8 fn, u16 off) {
    return (volatile u32 *)(ecam + (((usize)(bus - bus_first) << 20) | ((usize)dev << 15) | ((usize)fn << 12) | (off & 0xffc)));
}

int pci_available(void) { return ecam != NULL; }
u32 pci_read32(u8 bus, u8 dev, u8 fn, u16 off) { return ecam ? *cfg(bus, dev, fn, off) : 0xffffffffu; }
void pci_write32(u8 bus, u8 dev, u8 fn, u16 off, u32 v) { if (ecam) *cfg(bus, dev, fn, off) = v; }
u16 pci_read16(u8 bus, u8 dev, u8 fn, u16 off) { return (u16)(pci_read32(bus, dev, fn, off & ~3) >> ((off & 2) * 8)); }

u64 pci_bar(u8 bus, u8 dev, u8 fn, int bar) {
    u32 lo = pci_read32(bus, dev, fn, 0x10 + bar * 4);
    if (lo & 1) return 0;                               /* I/O BAR */
    u64 a = lo & ~0xfu;
    if ((lo & 6) == 4) a |= (u64)pci_read32(bus, dev, fn, 0x14 + bar * 4) << 32;
    return a;
}

int pci_find_cap(u8 bus, u8 dev, u8 fn, u8 id) {
    if (!(pci_read16(bus, dev, fn, 0x06) & 0x10)) return 0;
    u8 p = (u8)pci_read32(bus, dev, fn, 0x34) & 0xfc;
    for (int guard = 0; p && guard < 48; guard++) {
        u32 v = pci_read32(bus, dev, fn, p);
        if ((v & 0xff) == id) return p;
        p = (u8)(v >> 8) & 0xfc;
    }
    return 0;
}

void pci_set_driver(u8 bus, u8 dev, u8 fn, const char *name) {
    for (int i = 0; i < pci_ndevs; i++)
        if (pci_devs[i].bus == bus && pci_devs[i].dev == dev && pci_devs[i].fn == fn) pci_devs[i].driver = name;
}

static void scan_bus(u8 bus, int depth) {
    for (u8 dev = 0; dev < 32; dev++) {
        for (u8 fn = 0; fn < 8; fn++) {
            u32 id = pci_read32(bus, dev, fn, 0);
            if ((id & 0xffff) == 0xffff) { if (fn == 0) break; continue; }
            u32 cls = pci_read32(bus, dev, fn, 8);
            if (pci_ndevs < PCI_MAX_DEVS) {
                pci_dev_t *d = &pci_devs[pci_ndevs++];
                d->bus = bus; d->dev = dev; d->fn = fn;
                d->vendor = (u16)id; d->device = (u16)(id >> 16);
                d->class_code = (u8)(cls >> 24); d->subclass = (u8)(cls >> 16); d->prog_if = (u8)(cls >> 8);
                d->driver = NULL;
            }
            u8 hdr = (u8)(pci_read32(bus, dev, fn, 0x0c) >> 16);
            if ((hdr & 0x7f) == 1 && depth < 8) {          /* PCI-to-PCI bridge: follow secondary bus */
                u8 sec = (u8)(pci_read32(bus, dev, fn, 0x18) >> 8);
                if (sec > bus && sec <= bus_last) scan_bus(sec, depth + 1);
            }
            if (fn == 0 && !(hdr & 0x80)) break;           /* single-function device */
        }
    }
}

int pci_init(void) {
    if (ecam) return pci_ndevs;
    const u8 *m = acpi_table("MCFG", 0);
    if (!m || *(const u32 *)(m + 4) < 44 + 16) { klog("pci: no MCFG table"); return 0; }
    const u8 *e = m + 44;                              /* first allocation entry */
    u64 base = *(const u64 *)e;
    bus_first = e[10];
    bus_last = e[11];
    if (sizeof(void *) == 4 && base >> 32) return 0;
    ecam = (volatile u8 *)(usize)base;
    if (!ecam) return 0;
    pci_ndevs = 0;
    scan_bus(bus_first, 0);
    klog("pci: ECAM at %llx, buses %u-%u, %d functions", base, bus_first, bus_last, pci_ndevs);
    return pci_ndevs;
}
