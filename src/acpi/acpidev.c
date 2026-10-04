/*
 * acpidev.c - what Linux's ACPI drivers do on a PC, on top of ACPICA (src/acpi/acpica):
 *
 *   bring-up   drivers/acpi/bus.c: tables, the embedded controller's address space,
 *              ACPI mode, _INI/_STA, every GPE that has a method (acpi_update_all_gpes);
 *   EC         drivers/acpi/ec.c, polled: the EmbeddedControl address space (read 0x80,
 *              write 0x81) and its events (query 0x84, then the _Qxx method);
 *   battery    drivers/acpi/battery.c: _STA, _BIX or _BIF, _BST (mW converted to mAh);
 *   AC         drivers/acpi/ac.c: _PSR;      lid  drivers/acpi/button.c: _LID;
 *   buttons    the power button (fixed event or PNP0C0C), the sleep button (PNP0C0E);
 *   hotkeys    drivers/platform/x86/panasonic-laptop.c: Notify(HKEY, 0x80), then HINF
 *              gives the key (bit 7: pressed); ACPI video brightness notifies 0x86/0x87.
 *
 * Everything ACPI runs in one kernel thread.  The SCI stays masked: the thread calls
 * ACPICA's SCI handler every 20 ms (fixed events, GPEs) and runs the work ACPICA defers
 * (GPE methods, Notify handlers).  Every Notify is logged, so a button QRT does not
 * know yet shows its device and code in the System app's log.
 */
#include "acpidev.h"
#include "../arch/x64/sched.h"
#include "../drivers/backlight.h"
#include "acpi.h"

void AcpiOsQrtRunDeferred(void);
UINT32 AcpiOsQrtPollSci(void);

static char status[160] = "not started";
static volatile int ready;

/* ---- events for the shell ---------------------------------------------------------------- */
static event_t evq[32];
static volatile int evn;
static volatile int evlock;
static void post_key(u16 scan) {
    while (__atomic_exchange_n(&evlock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    if (evn < (int)ARRAY_LEN(evq)) evq[evn++] = (event_t){ .type = EV_KEY, .scan = scan };
    __atomic_store_n(&evlock, 0, __ATOMIC_RELEASE);
}
int acpi_poll(event_t *out, int max) {
    if (!evn) return 0;
    while (__atomic_exchange_n(&evlock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    int n = 0;
    while (n < max && n < evn) { out[n] = evq[n]; n++; }
    for (int i = n; i < evn; i++) evq[i - n] = evq[i];
    evn -= n;
    __atomic_store_n(&evlock, 0, __ATOMIC_RELEASE);
    return n;
}

static void brightness_step(int d) {
    if (!backlight_available()) return;
    backlight_set_level(CLAMP(backlight_level() + d, 5, 100));
}

/* ---- helpers ------------------------------------------------------------------------------ */
static int eval_int(ACPI_HANDLE h, const char *name, u64 *out) {
    ACPI_OBJECT o;
    ACPI_BUFFER b = { sizeof o, &o };
    if (ACPI_FAILURE(AcpiEvaluateObjectTyped(h, (char *)name, NULL, &b, ACPI_TYPE_INTEGER))) return -1;
    *out = o.Integer.Value;
    return 0;
}
static void path_of(ACPI_HANDLE h, char *out, usize cap) {
    ACPI_BUFFER b = { cap, out };
    if (ACPI_FAILURE(AcpiGetName(h, ACPI_FULL_PATHNAME, &b))) strlcpy(out, "?", cap);
}
static int hid_of(ACPI_HANDLE h, char *out, usize cap) {
    ACPI_DEVICE_INFO *info = NULL;
    out[0] = 0;
    if (ACPI_FAILURE(AcpiGetObjectInfo(h, &info))) return 0;
    if (info->Valid & ACPI_VALID_HID) strlcpy(out, info->HardwareId.String, cap);
    ACPI_FREE(info);
    return out[0] != 0;
}

/* ---- the embedded controller (drivers/acpi/ec.c, polled) ----------------------------------- */
#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_SCI_EVT 0x20
static struct { int found; u16 data, cmd; u32 gpe; int glk; ACPI_HANDLE h; u64 errors; } ec;

static inline u8 inb_(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb_(u16 p, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }

static int ec_wait(u8 mask, u8 want) {
    for (int i = 0; i < 20000; i++) {                       /* up to ~200 ms, as Linux's ec_delay */
        if ((inb_(ec.cmd) & mask) == want) return 0;
        hal_delay_us(10);
    }
    ec.errors++;
    return -1;
}
static int ec_transaction(u8 command, const u8 *wdata, int wlen, u8 *rdata, int rlen) {
    u32 glk = 0;
    if (ec.glk && ACPI_FAILURE(AcpiAcquireGlobalLock(1000, &glk))) return -1;
    int err = ec_wait(EC_IBF, 0);
    if (!err) { outb_(ec.cmd, command); err = ec_wait(EC_IBF, 0); }
    for (int i = 0; !err && i < wlen; i++) { outb_(ec.data, wdata[i]); err = ec_wait(EC_IBF, 0); }
    for (int i = 0; !err && i < rlen; i++) { err = ec_wait(EC_OBF, EC_OBF); if (!err) rdata[i] = inb_(ec.data); }
    if (ec.glk) AcpiReleaseGlobalLock(glk);
    return err;
}
static ACPI_STATUS ec_space(UINT32 fn, ACPI_PHYSICAL_ADDRESS addr, UINT32 bits, UINT64 *value, void *hctx, void *rctx) {
    if (addr > 0xff || bits % 8 || !value) return AE_BAD_PARAMETER;
    int bytes = (int)(bits / 8);
    u8 *v = (u8 *)value;
    if (fn == ACPI_READ) *value = 0;
    for (int i = 0; i < bytes; i++) {
        u8 a = (u8)(addr + i);
        int err = fn == ACPI_READ ? ec_transaction(0x80, &a, 1, &v[i], 1) : ec_transaction(0x81, (u8[]){ a, v[i] }, 2, NULL, 0);
        if (err) return AE_TIME;
    }
    return AE_OK;
}

static ACPI_STATUS ec_crs(ACPI_RESOURCE *r, void *ctx) {
    int *n = ctx;
    if (r->Type == ACPI_RESOURCE_TYPE_IO) {
        if (*n == 0) ec.data = r->Data.Io.Minimum;
        else if (*n == 1) ec.cmd = r->Data.Io.Minimum;
        (*n)++;
    } else if (r->Type == ACPI_RESOURCE_TYPE_FIXED_IO) {
        if (*n == 0) ec.data = r->Data.FixedIo.Address;
        else if (*n == 1) ec.cmd = r->Data.FixedIo.Address;
        (*n)++;
    }
    return AE_OK;
}
static ACPI_STATUS ec_found(ACPI_HANDLE h, UINT32 lvl, void *ctx, void **ret) {
    if (ec.found) return AE_CTRL_TERMINATE;
    int n = 0;
    if (ACPI_FAILURE(AcpiWalkResources(h, METHOD_NAME__CRS, ec_crs, &n)) || n < 2) return AE_OK;
    u64 v;
    if (!eval_int(h, "_GPE", &v)) ec.gpe = (u32)v; else ec.gpe = ~0u;
    if (!eval_int(h, "_GLK", &v)) ec.glk = v != 0;
    ec.h = h;
    ec.found = 1;
    return AE_CTRL_TERMINATE;
}

static int ecdt_probe(void) {                               /* acpi_ec_ecdt_probe: the EC before the namespace */
    ACPI_TABLE_HEADER *t;
    if (ACPI_FAILURE(AcpiGetTable(ACPI_SIG_ECDT, 1, &t))) return 0;
    ACPI_TABLE_ECDT *e = (ACPI_TABLE_ECDT *)t;
    if (!e->Control.Address || !e->Data.Address) return 0;
    ec.cmd = (u16)e->Control.Address;
    ec.data = (u16)e->Data.Address;
    ec.gpe = e->Gpe;
    ec.found = 1;
    if (ACPI_FAILURE(AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT, ACPI_ADR_SPACE_EC, ec_space, NULL, NULL))) { ec.found = 0; return 0; }
    klog("acpi: embedded controller from the ECDT: ports 0x%x/0x%x, GPE %u", ec.data, ec.cmd, ec.gpe);
    return 1;
}

static void ec_events(void) {                               /* acpi_ec_event_handler: _Qxx for each query */
    if (!ec.found || !ec.h) return;
    for (int i = 0; i < 8 && (inb_(ec.cmd) & EC_SCI_EVT); i++) {
        u8 q = 0;
        if (ec_transaction(0x84, NULL, 0, &q, 1) || !q) return;
        char name[5] = { '_', 'Q', "0123456789ABCDEF"[q >> 4], "0123456789ABCDEF"[q & 15], 0 };
        ACPI_STATUS st = AcpiEvaluateObject(ec.h, name, NULL, NULL);
        if (ACPI_FAILURE(st) && st != AE_NOT_FOUND) klog("acpi: EC query %s: %s", name, AcpiFormatException(st));
        AcpiOsQrtRunDeferred();                            /* the Notify()s it made */
    }
}
static UINT32 ec_gpe(ACPI_HANDLE dev, UINT32 gpe, void *ctx) { return ACPI_INTERRUPT_HANDLED | ACPI_REENABLE_GPE; }

/* ---- devices -------------------------------------------------------------------------------- */
#define MAXDEV 8
static ACPI_HANDLE batteries[MAXDEV], acs[MAXDEV], lids[MAXDEV], hkeys[MAXDEV];
static int nbat, nac, nlid, nhkey;
static battery_t bat;
static int have_power;
static volatile int bat_dirty = 1;

static ACPI_STATUS collect(ACPI_HANDLE h, UINT32 lvl, void *ctx, void **ret) {
    ACPI_HANDLE *list = ctx;
    int *n = list == batteries ? &nbat : list == acs ? &nac : list == lids ? &nlid : &nhkey;
    if (*n < MAXDEV) list[(*n)++] = h;
    return AE_OK;
}

static u64 pkg_int(ACPI_OBJECT *p, u32 i) {
    if (!p || p->Type != ACPI_TYPE_PACKAGE || i >= p->Package.Count) return ~0ull;
    ACPI_OBJECT *e = &p->Package.Elements[i];
    return e->Type == ACPI_TYPE_INTEGER ? e->Integer.Value : ~0ull;
}

static void read_battery(void) {                            /* drivers/acpi/battery.c */
    battery_t b = { 0 };
    b.minutes = -1;
    for (int i = 0; i < nbat && !b.present; i++) {
        u64 sta = 0x1f;
        eval_int(batteries[i], "_STA", &sta);
        if (!(sta & 0x10)) continue;                       /* no battery in this bay */
        ACPI_BUFFER info = { ACPI_ALLOCATE_BUFFER, NULL }, st = { ACPI_ALLOCATE_BUFFER, NULL };
        int bix = 1;
        if (ACPI_FAILURE(AcpiEvaluateObjectTyped(batteries[i], "_BIX", NULL, &info, ACPI_TYPE_PACKAGE))) {
            bix = 0;
            if (ACPI_FAILURE(AcpiEvaluateObjectTyped(batteries[i], "_BIF", NULL, &info, ACPI_TYPE_PACKAGE))) continue;
        }
        if (ACPI_FAILURE(AcpiEvaluateObjectTyped(batteries[i], "_BST", NULL, &st, ACPI_TYPE_PACKAGE))) { ACPI_FREE(info.Pointer); continue; }
        ACPI_OBJECT *pi = info.Pointer, *ps = st.Pointer;
        int o = bix ? 1 : 0;                               /* _BIX has a revision first */
        u64 unit = pkg_int(pi, (u32)o), design = pkg_int(pi, (u32)o + 1), full = pkg_int(pi, (u32)o + 2), dv = pkg_int(pi, (u32)o + 4);
        u64 state = pkg_int(ps, 0), rate = pkg_int(ps, 1), rem = pkg_int(ps, 2), volt = pkg_int(ps, 3);
        ACPI_FREE(info.Pointer); ACPI_FREE(st.Pointer);
        #define KNOWN(v) ((v) != ~0ull && (v) != 0xffffffffull)
        u64 mv = KNOWN(volt) && volt ? volt : KNOWN(dv) ? dv : 0;
        if (unit == 0 && mv) {                             /* mW and mWh: to mA and mAh */
            if (KNOWN(design)) design = design * 1000 / mv;
            if (KNOWN(full)) full = full * 1000 / mv;
            if (KNOWN(rem)) rem = rem * 1000 / mv;
            if (KNOWN(rate)) rate = rate * 1000 / mv;
        }
        b.present = 1;
        b.discharging = (state & 1) != 0;
        b.charging = (state & 2) != 0;
        b.critical = (state & 4) != 0;
        b.mv = KNOWN(volt) ? (int)volt : 0;
        b.ma = KNOWN(rate) ? (int)rate : 0;
        b.mah = KNOWN(rem) ? (int)rem : 0;
        b.full_mah = KNOWN(full) && full ? (int)full : KNOWN(design) ? (int)design : 0;
        b.design_mah = KNOWN(design) ? (int)design : 0;
        b.percent = b.full_mah ? CLAMP(b.mah * 100 / b.full_mah, 0, 100) : 0;
        if (b.ma > 0 && b.discharging) b.minutes = b.mah * 60 / b.ma;
        else if (b.ma > 0 && b.charging && b.full_mah > b.mah) b.minutes = (b.full_mah - b.mah) * 60 / b.ma;
    }
    for (int i = 0; i < nac; i++) { u64 v; if (!eval_int(acs[i], "_PSR", &v)) b.ac = v != 0; }
    for (int i = 0; i < nlid; i++) { u64 v; if (!eval_int(lids[i], "_LID", &v)) b.lid_closed = v == 0; }
    if (!b.present && !nbat) b.ac = nac ? b.ac : 1;
    bat = b;
    have_power = nbat || nac;
}

int acpi_battery_fill(battery_t *b, char *st, usize cap) {
    if (!ready || !have_power) return 0;
    *b = bat;
    if (bat.present)
        fmt(st, cap, "ACPI battery: %d%%, %s, %d/%d mAh (design %d), %d mV%s", bat.percent,
            bat.charging ? "charging" : bat.discharging ? "discharging" : "full", bat.mah, bat.full_mah, bat.design_mah, bat.mv,
            bat.ac ? ", on AC" : "");
    else fmt(st, cap, "ACPI: no battery%s", bat.ac ? ", on AC" : "");
    return 1;
}

/* ---- notifications (drivers/acpi/bus.c acpi_bus_notify and the drivers above) ----------- */
static void hotkey(ACPI_HANDLE h) {                         /* panasonic-laptop: acpi_pcc_generate_keyinput */
    u64 r;
    if (eval_int(h, "HINF", &r)) { klog("acpi: panasonic: HINF failed"); return; }
    int key = (int)(r & 0x7f), down = (r & 0x80) != 0;
    klog("acpi: panasonic: key %d %s", key, down ? "down" : "up");
    if (!down && key != 7 && key != 10) return;
    switch (key) {
    case 1: brightness_step(-10); break;
    case 2: brightness_step(10); break;
    case 5: post_key(SCAN_VOLDN); break;
    case 6: post_key(SCAN_VOLUP); break;
    case 7: case 10: if (!down) post_key(SCAN_POWER); break;
    }
}

static void notify(ACPI_HANDLE h, UINT32 value, void *ctx) {
    char hid[16], path[64];
    hid_of(h, hid, sizeof hid);
    path_of(h, path, sizeof path);
    klog("acpi: Notify(%s [%s], 0x%x)", path, hid[0] ? hid : "-", value);
    if (!strcmp(hid, "PNP0C0A") || !strcmp(hid, "ACPI0003") || !strcmp(hid, "PNP0C0D")) { bat_dirty = 1; return; }
    if (!strcmp(hid, "PNP0C0C") && value == 0x80) { post_key(SCAN_POWER); return; }
    if (!strcmp(hid, "PNP0C0E") && value == 0x80) { post_key(SCAN_POWER); return; }
    if (!strncmp(hid, "MAT00", 5) && value == 0x80) { hotkey(h); return; }
    if (value == 0x86) { brightness_step(10); return; }   /* ACPI video: brightness up */
    if (value == 0x87) { brightness_step(-10); return; }  /* brightness down */
}

static UINT32 power_button(void *ctx) { post_key(SCAN_POWER); klog("acpi: power button"); return ACPI_INTERRUPT_HANDLED; }

/* ---- bring-up and the thread ------------------------------------------------------------------ */
#define TRY(what, call) do { ACPI_STATUS s_ = (call); if (ACPI_FAILURE(s_)) { fmt(status, sizeof status, "%s failed: %s", what, AcpiFormatException(s_)); klog("acpi: %s", status); return -1; } } while (0)

static int bringup(void) {
    TRY("AcpiInitializeSubsystem", AcpiInitializeSubsystem());
    TRY("AcpiInitializeTables", AcpiInitializeTables(NULL, 32, FALSE));
    TRY("AcpiLoadTables", AcpiLoadTables());
    ecdt_probe();
    TRY("AcpiEnableSubsystem", AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION));
    if (!ec.found) {                                        /* acpi_ec_dsdt_probe */
        AcpiGetDevices("PNP0C09", ec_found, NULL, NULL);
        if (ec.found) {
            if (ACPI_FAILURE(AcpiInstallAddressSpaceHandler(ec.h, ACPI_ADR_SPACE_EC, ec_space, NULL, NULL))) ec.found = 0;
            else klog("acpi: embedded controller: ports 0x%x/0x%x, GPE %d%s", ec.data, ec.cmd, (int)ec.gpe, ec.glk ? ", global lock" : "");
        }
    } else AcpiGetDevices("PNP0C09", ec_found, NULL, NULL);  /* the ECDT's EC: its node, for _Qxx */
    TRY("AcpiInitializeObjects", AcpiInitializeObjects(ACPI_FULL_INITIALIZATION));
    if (ec.found && ec.gpe != ~0u && ACPI_SUCCESS(AcpiInstallGpeHandler(NULL, ec.gpe, ACPI_GPE_EDGE_TRIGGERED, ec_gpe, NULL)))
        AcpiEnableGpe(NULL, ec.gpe);
    AcpiInstallNotifyHandler(ACPI_ROOT_OBJECT, ACPI_ALL_NOTIFY, notify, NULL);
    if (ACPI_SUCCESS(AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, power_button, NULL))) AcpiEnableEvent(ACPI_EVENT_POWER_BUTTON, 0);
    AcpiUpdateAllGpes();
    AcpiGetDevices("PNP0C0A", collect, batteries, NULL);
    AcpiGetDevices("ACPI0003", collect, acs, NULL);
    AcpiGetDevices("PNP0C0D", collect, lids, NULL);
    static const char *const pcc[] = { "MAT0012", "MAT0013", "MAT0018", "MAT0019" };
    for (int i = 0; i < 4; i++) AcpiGetDevices((char *)pcc[i], collect, hkeys, NULL);
    read_battery();
    fmt(status, sizeof status, "ACPICA %08x: %d batter%s, %d AC adapter%s, %d lid%s, %s, %s",
        ACPI_CA_VERSION, nbat, nbat == 1 ? "y" : "ies", nac, nac == 1 ? "" : "s", nlid, nlid == 1 ? "" : "s",
        ec.found ? "embedded controller" : "no embedded controller", nhkey ? "Panasonic hotkeys" : "no vendor hotkeys");
    klog("acpi: %s", status);
    return 0;
}

static void acpi_thread(void *arg) {
    u64 t0 = k_now_ms();
    if (bringup()) { thread_exit(); return; }
    klog("acpi: ready in %llu ms", k_now_ms() - t0);
    ready = 1;
    u64 next_power = 0, next_bat = k_now_ms() + 15000;
    for (;;) {
        AcpiOsQrtPollSci();
        ec_events();
        AcpiOsQrtRunDeferred();
        u64 now = k_now_ms();
        if (bat_dirty || now >= next_power) {
            if (bat_dirty || now >= next_bat) { read_battery(); next_bat = now + 15000; }
            else {                                           /* AC and lid every second, cheap */
                for (int i = 0; i < nac; i++) { u64 v; if (!eval_int(acs[i], "_PSR", &v) && bat.ac != (v != 0)) { bat.ac = v != 0; next_bat = 0; } }
                for (int i = 0; i < nlid; i++) { u64 v; if (!eval_int(lids[i], "_LID", &v)) bat.lid_closed = v == 0; }
            }
            bat_dirty = 0;
            next_power = now + 1000;
        }
        thread_sleep_ms(20);
    }
}

void acpi_start(void) {
    if (k.is_venue) { strlcpy(status, "not used on the Venue 8 Pro (its own drivers)", sizeof status); return; }
    if (!hal_setting_get(u"QrtAcpi", 1)) { strlcpy(status, "turned off (QrtAcpi = 0)", sizeof status); return; }
    strlcpy(status, "starting", sizeof status);
    thread_create("acpi", acpi_thread, NULL, 0);
}

const char *acpi_status(void) { return status; }
