/*
 * dev.c - device list and driver binding (see dev.h).
 */
#include "dev.h"

device_t devs[DEV_MAX];
int n_devs;

/* Plain-language names for ids this hardware family is known to carry, so
 * an unbound device reads as a to-do item rather than a code. */
static const struct { const char *id, *what; } known[] = {
    { "PNP0A08", "PCI Express root" },           { "PNP0A03", "PCI root" },
    { "PNP0C50", "HID over I2C (touchscreen)" }, { "PNP0501", "16550 serial port" },
    { "PNP0C0C", "power button" },               { "PNP0C0E", "sleep button" },
    { "PNP0C0D", "lid switch" },                 { "PNP0C0A", "battery" },
    { "ACPI0003", "AC adapter" },                { "PNP0C09", "embedded controller" },
    { "PNP0103", "HPET timer" },                 { "PNP0B00", "RTC" },
    { "PNP0303", "PS/2 keyboard" },              { "PNP0F13", "PS/2 mouse" },
    { "PNP0C14", "WMI" },                        { "ACPI0007", "processor" },
    { "ACPI0011", "GPIO buttons (volume, home)" },
    { "INT33FC", "Bay Trail GPIO" },             { "INT33FF", "Cherry Trail GPIO" },
    { "80860F14", "SD/eMMC host (SDHCI)" },      { "80860F0A", "LPSS UART" },
    { "8086228A", "LPSS UART" },                 { "80860F41", "LPSS I2C" },
    { "808622C1", "LPSS I2C" },                  { "80860F28", "LPE audio DSP" },
    { "808622A8", "LPE audio DSP" },             { "10EC5640", "RT5640 audio codec" },
    { "10EC5645", "RT5645 audio codec" },        { "INT3496", "USB OTG id pin" },
    { "80860F09", "LPSS PWM (backlight)" },      { "80862288", "LPSS PWM (backlight)" },
    { "INT33F4", "PMIC" },                       { "INT33FD", "Crystal Cove PMIC" },
    { "INT34D3", "Whiskey Cove PMIC" },          { "INT0002", "virtual GPIO" },
    { "PNP0C0F", "PCI interrupt link" },         { "PNP0C02", "motherboard resources" },
    { "PNP0000", "8259 PIC" },                   { "PNP0100", "8254 timer" },
    { "PNP0200", "DMA controller" },             { "PNP0800", "PC speaker" },
    { "PNP0C01", "system board" },               { "PNP0C0B", "fan" },
    { "QEMU0002", "QEMU fw_cfg" },               { "PNP0A06", "generic container" },
    { "PNP0A05", "generic container" },          { "ACPI0010", "processor container" },
    { "ACPI0006", "GPE block" },                 { "PNP0400", "parallel port" },
};

static const char *pci_what(const pci_dev_t *p) {
    switch (p->class_code) {
    case 0x01: return p->subclass == 0x06 ? "SATA (AHCI)" : p->subclass == 0x08 ? "NVMe" : "storage";
    case 0x02: return p->subclass == 0x80 ? "wireless network" : "network";
    case 0x03: return "display";
    case 0x04: return p->subclass == 0x03 ? "HD audio" : "multimedia";
    case 0x06: return p->subclass == 0x00 ? "host bridge" : p->subclass == 0x01 ? "ISA bridge" : p->subclass == 0x04 ? "PCI bridge" : "bridge";
    case 0x08: return p->subclass == 0x05 ? "SD host" : "system peripheral";
    case 0x0c: return p->subclass == 0x03 ? (p->prog_if == 0x30 ? "USB 3 (xHCI)" : "USB") :
                      p->subclass == 0x05 ? "SMBus" : p->subclass == 0x80 ? "LPSS (I2C/UART/SPI)" : "serial bus";
    case 0x10: return "crypto";
    case 0x11: return "signal processing";
    }
    return "device";
}

static int pci_matches(const pci_match_t *m, const pci_dev_t *p) {
    for (; m && m->vendor; m++)
        if ((m->vendor == PCI_ANY_ID || m->vendor == p->vendor) &&
            (m->device == PCI_ANY_ID || m->device == p->device) &&
            (m->class_code == PCI_ANY_CLS || m->class_code == p->class_code) &&
            (m->subclass == PCI_ANY_CLS || m->subclass == p->subclass)) return 1;
    return 0;
}

static int list_matches(const char *const *ids, const char *name) {
    for (; ids && *ids; ids++) if (!strcmp(*ids, name)) return 1;
    return 0;
}

static device_t *add(bus_kind_t bus, const char *name, const char *what) {
    if (n_devs >= DEV_MAX) { klog("dev: device table full, %s dropped", name); return NULL; }
    device_t *d = &devs[n_devs++];
    memset(d, 0, sizeof *d);
    d->bus = bus;
    strlcpy(d->name, name, sizeof d->name);
    d->what = what;
    return d;
}

static void bind(device_t *d) {
    for (const driver_t *const *dp = builtin_drivers; *dp; dp++) {
        const driver_t *drv = *dp;
        int match = d->bus == BUS_PCI ? pci_matches(drv->pci, d->pci)
                  : d->bus == BUS_ACPI ? list_matches(drv->acpi, d->name)
                  : list_matches(drv->platform, d->name);
        if (!match) continue;
        int r = drv->probe ? drv->probe(d) : 0;
        if (r == DEV_NOT_MINE) continue;
        d->drv = drv;
        if (r < 0) { d->failed = 1; klog("dev: %s: %s failed (%s)", d->name, drv->name, d->status); return; }
        if (d->bus == BUS_PCI) d->pci->driver = drv->name;
        klog("dev: %s -> %s%s%s", d->name, drv->name, d->status[0] ? ": " : "", d->status);
        return;
    }
}

void dev_init(void) {
    n_devs = 0;
    for (int i = 0; i < pci_ndevs; i++) {
        pci_dev_t *p = &pci_devs[i];
        char n[24];
        fmt(n, sizeof n, "%02x:%02x.%x", p->bus, p->dev, p->fn);
        device_t *d = add(BUS_PCI, n, pci_what(p));
        if (d) d->pci = p;
    }
    if (k.native) add(BUS_PLATFORM, "com1", "16550 serial port (legacy I/O 0x3f8)");
    const char *id;
    for (int i = 0; (id = hwreport_hid(i)); i++) {
        const char *what = NULL;
        for (usize j = 0; j < ARRAY_LEN(known); j++) if (!strcmp(known[j].id, id)) what = known[j].what;
        add(BUS_ACPI, id, what);
    }
    for (int i = 0; i < n_devs; i++) bind(&devs[i]);
    klog("dev: %d devices, %d bound to QRT drivers", n_devs, dev_bound());
}

void dev_refresh(void) {
    for (int i = 0; i < n_devs; i++)
        if (devs[i].drv && devs[i].drv->status && !devs[i].failed) devs[i].drv->status(&devs[i]);
}

int dev_bound(void) {
    int n = 0;
    for (int i = 0; i < n_devs; i++) n += devs[i].drv && !devs[i].failed;
    return n;
}
