/*
 * battery.c - the Venue 8 Pro 5855's battery, charger and lid.
 *
 * The DSDT reads all three from a small embedded controller on I2C3
 * (\_SB.PCI0.I2C3, PCI 00:18.3) at address 0x78, 100 kHz, through a
 * GenericSerialBus region ("WIEC").  AttribBytes(n) at offset o is an I2C
 * "read bytes": write the command o, repeated start, read n bytes:
 *   0x00, 16 bytes  ACFO:  AC in rate, AC present (bit 0), battery limits, lid, temperature
 *   0x10, 32 bytes  BTS1:  _BST/_BIX of the battery: state, percent, rate, voltage,
 *                          remaining, time, ..., full and design capacity, design voltage
 * The AML retries when the bytes sum to 0 (the controller was busy); so do we.
 * Little-endian words, mA / mV / mAh (_BIX says power unit 1, mA).
 */
#include "battery.h"
#include "dwi2c.h"
#include "pci.h"

#define EC_ADDR 0x78
static dwi2c_t bus;
static battery_t bat;
static char status[128] = "not probed";
static u64 next_bat;
static int tries_left = 3;

static int ec_read(u8 cmd, u8 *out, int n) {
    for (int attempt = 0; attempt < 3; attempt++) {
        int e = dwi2c_xfer(&bus, EC_ADDR, &cmd, 1, out, n);
        if (e) return e;
        int sum = 0;
        for (int i = 0; i < n; i++) sum += out[i];
        if (sum) return 0;
        hal_delay_us(5000);
    }
    return DW_ENODEV;
}

static int w16(const u8 *p) { return p[0] | p[1] << 8; }

static void read_acfo(void) {
    u8 a[16];
    if (ec_read(0x00, a, sizeof a)) return;
    bat.ac = a[1] & 1;
    bat.lid_closed = a[4] == 0;           /* _LID: 1 = open */
}

static void read_battery(void) {
    u8 b[32];
    int e = ec_read(0x10, b, sizeof b);
    if (e) { fmt(status, sizeof status, "the controller at I2C3 0x78 did not answer (%s)", dwi2c_strerror(e)); bat.present = 0; return; }
    bat.present = 1;
    int st = b[0];
    bat.discharging = st & 1; bat.charging = (st >> 1) & 1; bat.critical = (st >> 2) & 1;
    bat.percent = CLAMP(b[1], 0, 100);
    int rate = w16(b + 2);
    if (rate >= 0x8000) rate = 0xffff - rate;    /* as _BST does */
    bat.ma = rate;
    bat.mv = w16(b + 4);
    bat.mah = w16(b + 6);
    bat.full_mah = w16(b + 14);
    bat.design_mah = w16(b + 16);
    bat.minutes = -1;
    if (bat.ma > 20 && bat.discharging) bat.minutes = bat.mah * 60 / bat.ma;
    else if (bat.ma > 20 && bat.charging && bat.full_mah > bat.mah) bat.minutes = (bat.full_mah - bat.mah) * 60 / bat.ma;
    fmt(status, sizeof status, "%d%%, %s, %d mV, %d mA, %d of %d mAh (design %d)%s",
        bat.percent, bat.charging ? "charging" : bat.discharging ? "on battery" : bat.ac ? "full, on AC" : "idle",
        bat.mv, bat.ma, bat.mah, bat.full_mah, bat.design_mah, bat.lid_closed ? ", cover closed" : "");
}

void battery_probe(void) {
    if (!k.is_venue) { strlcpy(status, "not a Venue 8 Pro (no battery controller known)", sizeof status); return; }
    if (pci_available()) {                        /* D0 and memory decoding, as for the other LPSS I2C buses */
        int pm = pci_find_cap(0, 0x18, 3, 0x01);
        if (pm) pci_write32(0, 0x18, 3, (u16)(pm + 4), pci_read32(0, 0x18, 3, (u16)(pm + 4)) & ~3u);
        pci_write32(0, 0x18, 3, 0x04, pci_read32(0, 0x18, 3, 0x04) | 0x6);
    }
    int e = dwi2c_find(&bus, 0, 0x18, 3);
    if (e) { fmt(status, sizeof status, "I2C3 controller: %s", dwi2c_strerror(e)); klog("battery: %s", status); return; }
    dwi2c_standard_mode(&bus);                    /* the controller is a 100 kHz device */
    dwi2c_save(&bus);
    read_acfo();
    read_battery();
    next_bat = k_now_ms() + 15000;
    klog("battery: %s", status);
}

void battery_native_resume(void) {
    if (!bus.found) return;
    if (dwi2c_restore(&bus, 3)) { strlcpy(status, "I2C3 did not come back after the handover", sizeof status); bus.found = 0; return; }
    next_bat = 0;                                 /* read again at once */
    tries_left = 3;
}

void battery_poll(void) {
    if (!bus.found) return;
    u64 now = k_now_ms();
    if (!bat.present && now >= next_bat && tries_left <= 0) return;   /* gave up: no controller */
    read_acfo();
    if (now >= next_bat) {
        read_battery();
        if (!bat.present) tries_left--;
        next_bat = now + 15000;
    }
}

const battery_t *battery_get(void) { return &bat; }
void battery_status(char *buf, usize cap) { strlcpy(buf, status, cap); }
