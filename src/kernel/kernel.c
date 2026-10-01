/* kernel.c - Tessera entry point and boot sequence. */
#include "kernel.h"
#include "dev.h"
#include "smp.h"
#include "vfs.h"
#include "../drivers/pci.h"
#include "../drivers/uart.h"
#include "../drivers/touch.h"

#if defined(__x86_64__)
int  native_prepare(void);
void native_enter(void);
static int native_wanted(void);
#endif

kernel_t k;

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

u64 k_now_ms(void) { return (rdtsc() - k.tsc_boot) / k.tsc_per_ms; }
u64 k_now_us(void) { return (rdtsc() - k.tsc_boot) * 1000 / k.tsc_per_ms; }

void k_walltime(EFI_TIME *t) {
    memset(t, 0, sizeof *t);
    if (EFI_ERROR(k.rt->GetTime(t, NULL))) {
        /* no RTC: count from boot so the clock still ticks */
        u64 s = k_now_ms() / 1000;
        t->Year = 2026; t->Month = 1; t->Day = 1;
        t->Hour = (u8)(s / 3600 % 24); t->Minute = (u8)(s / 60 % 60); t->Second = (u8)(s % 60);
    }
}

static void calibrate_clock(void) {
    u64 a = rdtsc();
    k.bs->Stall(20000);              /* 20 ms, timed by the firmware */
    u64 b = rdtsc();
    k.tsc_per_ms = (b - a) / 20;
    if (!k.tsc_per_ms) k.tsc_per_ms = 1000000;
}

/*
 * Firmware on tablets often boots in "fast" mode and binds only the
 * drivers needed to find the boot loader.  Walk every handle and ask the
 * firmware to connect all drivers recursively (the equivalent of the EFI
 * shell's `connect -r`) so touch, USB, SD and eMMC controllers come up.
 */
static void connect_all_drivers(void) {
    UINTN n = 0;
    EFI_HANDLE *h = NULL;
    if (EFI_ERROR(k.bs->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &h))) return;
    for (UINTN i = 0; i < n; i++)
        if (!EFI_ERROR(k.bs->ConnectController(h[i], NULL, NULL, 1)))
            k.drivers_connected++;
    k.bs->FreePool(h);
    /* connecting can create new handles; count what exists now */
    if (!EFI_ERROR(k.bs->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &h))) {
        k.handles = (int)n;
        k.bs->FreePool(h);
    }
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    k.image = image;
    k.st = st;
    k.bs = st->BootServices;
    k.rt = st->RuntimeServices;
    k.tsc_boot = rdtsc();
    k.tsc_per_ms = 1000000;

    /* The firmware arms a 5-minute watchdog for boot loaders. We are the OS. */
    k.bs->SetWatchdogTimer(0, 0, 0, NULL);

    /* the boot log goes to the serial port and the log ring, not the screen:
     * black text on black until the shell's first frame */
    st->ConOut->SetAttribute(st->ConOut, 0x00);
    st->ConOut->ClearScreen(st->ConOut);
    klog("QRT %s (%s) - Tessera kernel", QRT_VERSION, QRT_ARCH);
    calibrate_clock();
    klog("clock: %llu kHz TSC", k.tsc_per_ms);
    {   /* Unix epoch at boot, from the RTC (days-from-civil) */
        EFI_TIME t;
        k_walltime(&t);
        i64 y = t.Year - (t.Month <= 2), era = (y >= 0 ? y : y - 399) / 400;
        i64 yoe = y - era * 400, mp = (t.Month + 9) % 12;
        i64 doy = (153 * mp + 2) / 5 + t.Day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        i64 days = era * 146097 + doe - 719468;
        k.epoch_at_boot = (u64)(days * 86400 + t.Hour * 3600 + t.Minute * 60 + t.Second) - k_now_ms() / 1000;
    }

    connect_all_drivers();
    klog("firmware: %d handles, %d controllers connected", k.handles, k.drivers_connected);

    sysinfo_probe();
    hal_probe();
    if (!k.gop) panic("no Graphics Output Protocol - cannot start the shell");
    if (!hwreport_save()) klog("hwreport: boot volume not writable, skipped");
    hal_settings_prepare();
    pci_init();
    vfs_load_boot_volume();

#if defined(__x86_64__)
    if (native_wanted()) {
        klog("boot: handing over from the firmware to the native kernel");
        native_enter();                       /* never returns */
    }
#endif
    smp_init();
    dev_init();                               /* firmware mode: drivers only describe what the firmware runs */
    shell_main();
    return EFI_SUCCESS;
}

#if defined(__x86_64__)
/*
 * Native mode is used when QRT can drive the machine's input itself (the
 * native touchscreen driver found its chip, or a serial console exists),
 * unless the user asked for firmware mode: setting QrtBootMode = 1, holding
 * any key or hardware button at boot, or a failed native boot last time
 * (QrtBootMode = 2, cleared once honoured).
 */
static int native_wanted(void) {
    u32 mode = hal_setting_get(u"QrtBootMode", 0);
    if (mode == 2) { hal_setting_set(u"QrtBootMode", 0); klog("boot: last native boot failed - firmware mode"); return 0; }
    if (mode == 1) { klog("boot: firmware mode selected in settings"); return 0; }
    EFI_INPUT_KEY key;
    if (!EFI_ERROR(k.st->ConIn->ReadKeyStroke(k.st->ConIn, &key))) { klog("boot: key held - firmware mode"); return 0; }
    ntouch_probe();
    if (!native_prepare()) return 0;
    if (nt.primary < 0 && !uart_present()) { klog("boot: no native input device - firmware mode"); return 0; }
    return 1;
}
#endif
