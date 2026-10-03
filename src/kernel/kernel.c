/* kernel.c - Tessera entry point and boot sequence. */
#include "kernel.h"
#include "../drivers/audio.h"
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
    time_init();                     /* the wall clock, from the RTC (time.c) */

    connect_all_drivers();
    klog("firmware: %d handles, %d controllers connected", k.handles, k.drivers_connected);

    sysinfo_probe();
    hal_probe();
    if (!k.gop) panic("no Graphics Output Protocol - cannot start the shell");
    if (!hwreport_save()) klog("hwreport: boot volume not writable, skipped");
    hal_settings_prepare();
    pci_init();
    vfs_load_boot_volume();

    audio_probe();                            /* while the firmware's PCI access still works */
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
static int fw_because(const char *why) {
    fmt(k.boot_note, sizeof k.boot_note, "Firmware mode: %s", why);
    klog("boot: %s - firmware mode", why);
    return 0;
}

static int native_wanted(void) {
    u32 mode = hal_setting_get(u"QrtBootMode", 0);
    if (mode == 2) { hal_setting_set(u"QrtBootMode", 0); return fw_because("the last native boot failed (once)"); }
    if (mode == 1) return fw_because("selected in Settings");
    /* a key held at boot asks for firmware mode; keystrokes the firmware still
     * had queued (boot menu, a button pressed while starting) do not count */
    EFI_INPUT_KEY key;
    while (!EFI_ERROR(k.st->ConIn->ReadKeyStroke(k.st->ConIn, &key))) {}
    k.bs->Stall(150000);
    if (!EFI_ERROR(k.st->ConIn->ReadKeyStroke(k.st->ConIn, &key))) return fw_because("a key was held at boot");
    ntouch_probe();
    if (!native_prepare()) return fw_because("the native kernel could not be prepared");
    if (nt.primary < 0 && !uart_present()) return fw_because("no touchscreen found for the native kernel");
    return 1;
}
#endif
