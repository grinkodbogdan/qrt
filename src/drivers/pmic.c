/*
 * pmic.c - the PMIC on I2C7 (\_SB.PCI0.I2C7, PCI 00:18.7).
 *
 * The DSDT describes four alternatives and picks one with PMID (GNVS): Crystal
 * Cove (INT33FD, 0x6E), TI Dollar Cove (INT33F5, 0x5E), X-Powers AXP288
 * (INT33F4, 0x34) and Whiskey Cove (INT34D3, 0x4F/0x5E/0x6E).  We ask the bus
 * instead, reading register 0 (the id) at each address.
 *
 * The SoC's power unit (P-unit) talks to the PMIC on the same bus, so every
 * transfer first takes the hardware semaphore, as Linux's
 * i2c-designware-baytrail.c does: through the IOSF sideband (the MCR/MDR/MCRX
 * registers of PCI 00:00.0), port 0x04, register 0x10E on Cherry Trail: write
 * the acquire bit (1), wait for the owned bit (0); release by writing 0.
 */
#include "pmic.h"
#include "dwi2c.h"
#include "pci.h"

static dwi2c_t bus;
static int kind, addr;
static char status[96] = "not probed";

#define MBI_MCR  0xd0
#define MBI_MDR  0xd4
#define MBI_MCRX 0xd8
#define MBI_PMC  0x04
#define SEM_REG  0x10e
static u32 mbi_read(u8 port, u32 reg) {
    pci_write32(0, 0, 0, MBI_MCRX, reg & 0xffffff00u);
    pci_write32(0, 0, 0, MBI_MCR, 0x10u << 24 | (u32)port << 16 | (reg & 0xff) << 8 | 0xf0);
    return pci_read32(0, 0, 0, MBI_MDR);
}
static void mbi_write(u8 port, u32 reg, u32 v) {
    pci_write32(0, 0, 0, MBI_MDR, v);
    pci_write32(0, 0, 0, MBI_MCRX, reg & 0xffffff00u);
    pci_write32(0, 0, 0, MBI_MCR, 0x11u << 24 | (u32)port << 16 | (reg & 0xff) << 8 | 0xf0);
}
static int sem_get(void) {
    if (!pci_available()) return -1;
    mbi_write(MBI_PMC, SEM_REG, 2);
    u64 end = k_now_ms() + 50;
    while (!(mbi_read(MBI_PMC, SEM_REG) & 1)) if (k_now_ms() > end) { mbi_write(MBI_PMC, SEM_REG, 0); return -1; }
    return 0;
}
static void sem_put(void) { mbi_write(MBI_PMC, SEM_REG, 0); }

static int xfer(u8 a, const u8 *w, int wl, u8 *r, int rl) {
    if (!bus.found) return DW_ENODEV;
    if (sem_get()) return DW_ETIMEOUT;
    int e = dwi2c_xfer(&bus, a, w, wl, r, rl);
    sem_put();
    return e;
}

void pmic_probe(void) {
    if (!k.is_venue) { strlcpy(status, "not probed on this machine", sizeof status); return; }
    int e = dwi2c_find(&bus, 0, 0x18, 7);
    if (e) { fmt(status, sizeof status, "I2C7 controller: %s", dwi2c_strerror(e)); klog("pmic: %s", status); return; }
    dwi2c_save(&bus);
    /* Whiskey Cove answers at 0x4F (and 0x5E, 0x6E): look for it first */
    static const struct { u8 a, main; int kind; const char *name; } cand[] = {
        { 0x4f, 0x6e, PMIC_WHISKEY_COVE, "Whiskey Cove" }, { 0x6e, 0x6e, PMIC_CRYSTAL_COVE, "Crystal Cove" },
        { 0x5e, 0x5e, PMIC_DOLLAR_COVE_TI, "TI Dollar Cove" }, { 0x34, 0x34, PMIC_XPOWER, "X-Powers AXP288" },
    };
    for (usize i = 0; i < ARRAY_LEN(cand); i++) {
        u8 reg = 0, id = 0;
        if (xfer(cand[i].a, &reg, 1, &id, 1)) continue;
        kind = cand[i].kind; addr = cand[i].main;
        fmt(status, sizeof status, "%s at I2C7 0x%02x (id %02x)", cand[i].name, addr, id);
        klog("pmic: %s", status);
        return;
    }
    strlcpy(status, "no PMIC answered on I2C7", sizeof status);
    klog("pmic: %s", status);
}

void pmic_native_resume(void) {
    if (bus.found && dwi2c_restore(&bus, 7)) { strlcpy(status, "I2C7 did not come back after the handover", sizeof status); bus.found = 0; kind = 0; }
}

int pmic_kind(void) { return kind; }
int pmic_read(u8 reg, u8 *val) { return kind ? xfer((u8)addr, &reg, 1, val, 1) : DW_ENODEV; }
int pmic_write(u8 reg, u8 val) { u8 b[2] = { reg, val }; return kind ? xfer((u8)addr, b, 2, NULL, 0) : DW_ENODEV; }
const char *pmic_status(void) { return status; }
