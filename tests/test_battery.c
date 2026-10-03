/* Host test for src/drivers/battery.c: a simulated embedded controller at
 * I2C3 0x78 answering the two reads the DSDT makes (AC/lid at 0x00, the
 * battery at 0x10), busy now and then (all-zero answers, retried).
 * Build/run: make check */
#include <stdio.h>
#include "../src/drivers/battery.c"

kernel_t k;
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
void klog(const char *f, ...) { (void)f; }
void hal_delay_us(u32 us) { (void)us; }
static u64 now;
u64 k_now_ms(void) { return now; }
int pci_available(void) { return 0; }
u32 pci_read32(u8 b, u8 d, u8 f, u16 o) { (void)b; (void)d; (void)f; (void)o; return 0; }
void pci_write32(u8 b, u8 d, u8 f, u16 o, u32 v) { (void)b; (void)d; (void)f; (void)o; (void)v; }
int pci_find_cap(u8 b, u8 d, u8 f, u8 c) { (void)b; (void)d; (void)f; (void)c; return 0; }

/* the controller */
static u8 acfo[16], bts[32];
static int busy;                  /* answer this many reads with zeros */
static int reads;
int dwi2c_find(dwi2c_t *c, u32 b, u32 d, u32 f) { (void)b; (void)d; (void)f; c->found = 1; return 0; }
void dwi2c_standard_mode(dwi2c_t *c) { (void)c; }
void dwi2c_save(dwi2c_t *c) { c->has_saved = 1; }
int dwi2c_restore(dwi2c_t *c, u8 fn) { (void)c; (void)fn; return 0; }
const char *dwi2c_strerror(int e) { return e ? "error" : "ok"; }
int dwi2c_xfer(dwi2c_t *c, u8 addr, const u8 *w, int wlen, u8 *r, int rlen) {
    (void)c;
    if (addr != 0x78 || wlen != 1) return DW_EABORT;
    reads++;
    if (busy) { busy--; memset(r, 0, (usize)rlen); return 0; }
    if (w[0] == 0x00 && rlen == 16) { memcpy(r, acfo, 16); return 0; }
    if (w[0] == 0x10 && rlen == 32) { memcpy(r, bts, 32); return 0; }
    return DW_EABORT;
}

static void put16(u8 *p, int v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    k.is_venue = 1;
    /* on battery, 63 %, 640 mA drawn (the controller reports it as 0xffff - 640) */
    acfo[1] = 0; acfo[4] = 1;
    bts[0] = 1; bts[1] = 63; put16(bts + 2, 0xffff - 640); put16(bts + 4, 3850); put16(bts + 6, 2520);
    put16(bts + 14, 4000); put16(bts + 16, 4400); put16(bts + 18, 3800);
    busy = 1;                                         /* the first read finds it busy */
    battery_probe();
    const battery_t *b = battery_get();
    CHECK(b->present, "not present");
    CHECK(b->percent == 63 && b->discharging && !b->charging && !b->ac, "state %d%% d%d c%d ac%d", b->percent, b->discharging, b->charging, b->ac);
    CHECK(b->ma == 640 && b->mv == 3850 && b->mah == 2520 && b->full_mah == 4000 && b->design_mah == 4400, "numbers %d %d %d %d %d", b->ma, b->mv, b->mah, b->full_mah, b->design_mah);
    CHECK(b->minutes == 236, "minutes to empty %d", b->minutes);
    CHECK(!b->lid_closed, "cover closed");
    char st[128]; battery_status(st, sizeof st);
    printf("  %s\n", st);

    /* plugged in, charging, cover closed; the battery is read again only after 15 s */
    acfo[1] = 1; acfo[4] = 0;
    bts[0] = 2; bts[1] = 64; put16(bts + 2, 900);
    now = 2000; battery_poll();
    CHECK(b->ac && b->lid_closed && b->percent == 63, "AC/cover first, battery later: ac%d lid%d %d%%", b->ac, b->lid_closed, b->percent);
    now = 16000; battery_poll();
    CHECK(b->charging && b->percent == 64 && b->ma == 900 && b->minutes == (4000 - 2520) * 60 / 900, "charging %d %d%% %d mA %d min", b->charging, b->percent, b->ma, b->minutes);

    /* a controller that stays silent: three tries, then left alone */
    busy = 1000; reads = 0;
    for (int i = 0; i < 20; i++) { now += 16000; battery_poll(); }
    CHECK(!b->present, "silent controller still present");
    int r = reads; now += 16000; battery_poll();
    CHECK(reads == r, "kept polling a silent controller (%d reads)", reads - r);

    if (fails) { printf("test_battery: %d failures\n", fails); return 1; }
    printf("test_battery: battery, charger and cover from the embedded controller pass\n");
    return 0;
}
