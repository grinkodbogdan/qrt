/*
 * irq.c - I/O APIC routing and PCI MSI for device drivers.
 *
 * The MADT lists each I/O APIC (MMIO base, first GSI) and the interrupt
 * source overrides that move ISA IRQs to other GSIs or change their
 * polarity/trigger (on most PCs ISA IRQ 0 becomes GSI 2; on Cherry Trail the
 * SCI is level/active-low).  At init every redirection entry is masked, so
 * nothing the firmware left programmed can fire into QRT; a driver unmasks
 * exactly the line it attaches.
 */
#include "irq.h"
#include "arch.h"
#include "../../drivers/pci.h"

#define MAX_IOAPIC 4
#define MAX_LINES  32
#define VEC_FIRST  0x50
#define VEC_LAST   0xdf

static struct { volatile u32 *mmio; u32 gsi_base, count; } ioa[MAX_IOAPIC];
static int n_ioa;
static struct { u8 isa; u32 gsi; u16 flags; } iso[16];
static int n_iso;
static u32 dest_apic;

static irq_line_t lines[MAX_LINES];
static struct { dev_irq_fn fn; void *arg; } slot[MAX_LINES];
static int n_lines;
static int next_vec = VEC_FIRST;

static u32 ioa_read(int i, u32 reg) { ioa[i].mmio[0] = reg; return ioa[i].mmio[4]; }
static void ioa_write(int i, u32 reg, u32 v) { ioa[i].mmio[0] = reg; ioa[i].mmio[4] = v; }

static int ioa_for(u32 gsi, u32 *pin) {
    for (int i = 0; i < n_ioa; i++)
        if (gsi >= ioa[i].gsi_base && gsi < ioa[i].gsi_base + ioa[i].count) { *pin = gsi - ioa[i].gsi_base; return i; }
    return -1;
}

int irq_init(void) {
    const u8 *madt = acpi_table("APIC", 0);
    if (!madt) { klog("irq: no MADT - device interrupts unavailable, drivers poll"); return 0; }
    u32 len = *(const u32 *)(madt + 4);
    for (u32 off = 44; off + 2 <= len; off += madt[off + 1]) {
        const u8 *e = madt + off;
        if (e[1] < 2) break;
        if (e[0] == 1 && e[1] >= 12 && n_ioa < MAX_IOAPIC) {
            ioa[n_ioa].mmio = (volatile u32 *)(usize)*(const u32 *)(e + 4);
            ioa[n_ioa].gsi_base = *(const u32 *)(e + 8);
            n_ioa++;
        } else if (e[0] == 2 && e[1] >= 10 && n_iso < (int)ARRAY_LEN(iso)) {
            iso[n_iso].isa = e[3];
            iso[n_iso].gsi = *(const u32 *)(e + 4);
            iso[n_iso].flags = *(const u16 *)(e + 8);
            n_iso++;
        }
    }
    dest_apic = lapic_id();
    for (int i = 0; i < n_ioa; i++) {
        ioa[i].count = ((ioa_read(i, 1) >> 16) & 0xff) + 1;
        for (u32 p = 0; p < ioa[i].count; p++) {
            ioa_write(i, 0x10 + 2 * p, 1u << 16);          /* masked */
            ioa_write(i, 0x11 + 2 * p, 0);
        }
        klog("irq: I/O APIC %d at %p, GSI %u-%u, all masked", i, (void *)ioa[i].mmio,
             ioa[i].gsi_base, ioa[i].gsi_base + ioa[i].count - 1);
    }
    klog("irq: %d interrupt source overrides", n_iso);
    return n_ioa;
}

static void common(frame_t *f) {
    for (int i = 0; i < n_lines; i++)
        if (lines[i].vector == (int)f->vector) {
            lines[i].count++;
            slot[i].fn(slot[i].arg);
            return;
        }
}

static int new_line(u32 gsi, const char *owner, dev_irq_fn fn, void *arg) {
    if (n_lines >= MAX_LINES || next_vec > VEC_LAST) return -1;
    int i = n_lines;
    lines[i].vector = next_vec++;
    lines[i].gsi = gsi;
    lines[i].owner = owner;
    lines[i].count = 0;
    slot[i].fn = fn;
    slot[i].arg = arg;
    irq_register(lines[i].vector, common);
    __atomic_store_n(&n_lines, i + 1, __ATOMIC_RELEASE);
    return i;
}

int irq_attach_gsi(u32 gsi, int level, int active_low, const char *owner, dev_irq_fn fn, void *arg) {
    u32 pin;
    int a = ioa_for(gsi, &pin);
    if (a < 0) { klog("irq: GSI %u has no I/O APIC", gsi); return -1; }
    int i = new_line(gsi, owner, fn, arg);
    if (i < 0) return -1;
    u64 fl = irq_save();
    ioa_write(a, 0x11 + 2 * pin, dest_apic << 24);
    ioa_write(a, 0x10 + 2 * pin, (u32)lines[i].vector | (active_low ? 1u << 13 : 0) | (level ? 1u << 15 : 0));
    irq_restore(fl);
    klog("irq: GSI %u (%s, %s) -> vector 0x%x for %s", gsi, level ? "level" : "edge",
         active_low ? "active low" : "active high", lines[i].vector, owner);
    return lines[i].vector;
}

int irq_attach_isa(int isa_irq, const char *owner, dev_irq_fn fn, void *arg) {
    u32 gsi = (u32)isa_irq;
    int level = 0, low = 0;                 /* ISA default: edge, active high */
    for (int i = 0; i < n_iso; i++)
        if (iso[i].isa == isa_irq) {
            gsi = iso[i].gsi;
            if ((iso[i].flags & 3) == 3) low = 1;
            if (((iso[i].flags >> 2) & 3) == 3) level = 1;
        }
    return irq_attach_gsi(gsi, level, low, owner, fn, arg);
}

void irq_mask_gsi(u32 gsi, int masked) {
    u32 pin;
    int a = ioa_for(gsi, &pin);
    if (a < 0) return;
    u64 fl = irq_save();
    u32 lo = ioa_read(a, 0x10 + 2 * pin);
    ioa_write(a, 0x10 + 2 * pin, masked ? lo | 1u << 16 : lo & ~(1u << 16));
    irq_restore(fl);
}

/* MSI: the device writes 'data' (our vector) to the local APIC's address
 * window, so no I/O APIC is involved and the line is never shared. */
int irq_attach_msi(u8 bus, u8 dev, u8 fn, const char *owner, dev_irq_fn h, void *arg) {
    int cap = pci_find_cap(bus, dev, fn, 0x05);
    if (!cap) return -1;
    int i = new_line(~0u, owner, h, arg);
    if (i < 0) return -1;
    u32 ctl = pci_read32(bus, dev, fn, (u16)cap);
    int is64 = (ctl >> 16) & 0x80;
    pci_write32(bus, dev, fn, (u16)(cap + 4), 0xfee00000u | (dest_apic << 12));
    if (is64) {
        pci_write32(bus, dev, fn, (u16)(cap + 8), 0);
        pci_write32(bus, dev, fn, (u16)(cap + 12), (u32)lines[i].vector);
    } else {
        pci_write32(bus, dev, fn, (u16)(cap + 8), (u32)lines[i].vector);
    }
    ctl &= ~(7u << 20);                     /* one vector */
    ctl |= 1u << 16;                        /* MSI enable */
    pci_write32(bus, dev, fn, (u16)cap, ctl);
    u32 cmd = pci_read32(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, (cmd & 0xffff) | (1u << 10));   /* INTx off; leave status bits alone */
    klog("irq: MSI for %02x:%02x.%x -> vector 0x%x for %s", bus, dev, fn, lines[i].vector, owner);
    return lines[i].vector;
}

int irq_lines(const irq_line_t **out) { *out = lines; return __atomic_load_n(&n_lines, __ATOMIC_ACQUIRE); }
int irq_ioapics(void) { return n_ioa; }
