/* kernel.c - Tessera entry point and boot sequence. */
#include "kernel.h"

kernel_t k;

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

u64 k_now_ms(void) { return (rdtsc() - k.tsc_boot) / k.tsc_per_ms; }

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

    st->ConOut->ClearScreen(st->ConOut);
    klog("QRT %s (%s) - Tessera kernel", QRT_VERSION, QRT_ARCH);
    calibrate_clock();
    klog("clock: %llu kHz TSC", k.tsc_per_ms);

    connect_all_drivers();
    klog("firmware: %d handles, %d controllers connected", k.handles, k.drivers_connected);

    sysinfo_probe();
    hal_probe();
    if (!k.gop) panic("no Graphics Output Protocol - cannot start the shell");

    shell_main();
    return EFI_SUCCESS;
}
