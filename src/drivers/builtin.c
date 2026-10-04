/*
 * builtin.c - the drivers linked into QRT, in match order.
 *
 * Each driver is a driver_t (see src/kernel/dev.h).  The ones here are small:
 * most of the work of the touchscreen stack lives in dwi2c.c / i2chid.c /
 * touch.c, and these entries connect it to the device list.
 */
#include "../kernel/dev.h"
#include "uart.h"
#include "touch.h"
#include "buttons.h"
#include "backlight.h"
#include "battery.h"
#include "pmic.h"
#include "ish.h"
#include "e1000.h"
#include "iwm/iwm.h"
#include "i915/gpu.h"
#include "usb/xhci.h"
#include "audio.h"
#include "speaker.h"
#include "hda.h"
#if defined(__x86_64__)
#include "../arch/x64/irq.h"
#endif

/* ---- chipset: devices the kernel itself takes care of ---------------------- */
static const pci_match_t chipset_pci[] = { { PCI_ANY_ID, PCI_ANY_ID, 0x06, PCI_ANY_CLS }, { 0 } };
static const char *const chipset_acpi[] = { "PNP0A08", "PNP0A03", "PNP0C02", "PNP0C01", "PNP0A06",
                                            "PNP0000", "PNP0100", "PNP0103", "ACPI0007", "PNP0C0F", "PNP0A05", "ACPI0010", "ACPI0006", NULL };
static int chipset_probe(device_t *d) {
    const char *s = "no driver needed";
    if (d->bus == BUS_ACPI) {
        if (!strcmp(d->name, "PNP0000")) s = k.native ? "masked; QRT uses the local and I/O APICs" : "left to the firmware";
        else if (!strcmp(d->name, "PNP0100") || !strcmp(d->name, "PNP0103"))
            s = k.native ? "unused; QRT times with the local APIC and TSC" : "left to the firmware";
        else if (!strcmp(d->name, "ACPI0007")) s = k.native ? "cores started by QRT (INIT/SIPI)" : "cores started through UEFI MP services";
        else if (!strcmp(d->name, "PNP0C0F")) s = "PCI interrupt link (not routed yet: drivers use MSI or poll)";
        else if (!strcmp(d->name, "PNP0A08") || !strcmp(d->name, "PNP0A03")) s = "PCI root; config space through ECAM";
        else if (!strcmp(d->name, "ACPI0006")) s = "GPE block (ACPI events not handled yet)";
    }
    strlcpy(d->status, s, sizeof d->status);
    return 0;
}
static const driver_t drv_chipset = { "chipset", chipset_pci, chipset_acpi, NULL, chipset_probe, NULL };

/* ---- Intel Gen8 graphics (Cherry Trail): the 3D engine presents the screen ---- */
static const pci_match_t gpu_pci[] = { { 0x8086, 0x22b0, 0x03, PCI_ANY_CLS }, { 0x8086, 0x22b1, 0x03, PCI_ANY_CLS },
                                       { 0x8086, 0x22b2, 0x03, PCI_ANY_CLS }, { 0x8086, 0x22b3, 0x03, PCI_ANY_CLS }, { 0 } };
static void gpu_dev_status(device_t *d) { fmt(d->status, sizeof d->status, "Intel HD Graphics (Gen8): %s", gpu_status()); }
static int gpu_dev_probe(device_t *d) {
    if (!k.native) return DEV_NOT_MINE;           /* under the firmware GOP keeps the display */
    gpu_probe(d->pci);
    gpu_dev_status(d);
    return 0;
}
static const driver_t drv_gpu = { "i915", gpu_pci, NULL, NULL, gpu_dev_probe, gpu_dev_status };

/* ---- USB 3 host controller (xHCI): root-port devices, keyboards ------------------ */
static const pci_match_t xhci_pci[] = { { PCI_ANY_ID, PCI_ANY_ID, 0x0c, 0x03 }, { 0 } };
static void xhci_dev_status(device_t *d) { xhci_status(d->status, sizeof d->status); }
static int xhci_dev_probe(device_t *d) {
    if (d->pci->prog_if != 0x30) return DEV_NOT_MINE;                 /* UHCI/OHCI/EHCI: not ours */
    if (!k.native) { strlcpy(d->status, "the firmware's USB driver runs it", sizeof d->status); return 0; }
    xhci_probe(d->pci);
    xhci_dev_status(d);
    return 0;
}
static const driver_t drv_xhci = { "xhci", xhci_pci, NULL, NULL, xhci_dev_probe, xhci_dev_status };

/* ---- sound: the Realtek codec (identified in audio.c) and Intel's SST DSP ---------- */
static const char *const codec_acpi[] = { "10EC5672", "10EC5670", "10EC5640", NULL };
static const pci_match_t sst_pci[] = { { 0x8086, 0x22a8, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x0f28, PCI_ANY_CLS, PCI_ANY_CLS }, { 0 } };
static int codec_owner;
static void codec_status(device_t *d) {
    if (d->bus == BUS_PCI) fmt(d->status, sizeof d->status, "Intel audio DSP (speakers): %s", speaker_status());
    else if (d->priv) strlcpy(d->status, audio_status(), sizeof d->status);
    else strlcpy(d->status, "another entry for the same codec slot", sizeof d->status);
}
static int codec_probe(device_t *d) {
    static int started;
    if (d->bus == BUS_ACPI && !codec_owner) { codec_owner = 1; d->priv = d; }
    if (!started && k.is_venue) { started = 1; speaker_start(); }   /* the DSP may be a PCI device or only in ACPI */
    codec_status(d);
    return 0;
}
static const driver_t drv_audio = { "audio", sst_pci, codec_acpi, NULL, codec_probe, codec_status };

/* ---- framebuffer --------------------------------------------------------- */
static const pci_match_t fb_pci[] = { { PCI_ANY_ID, PCI_ANY_ID, 0x03, PCI_ANY_CLS }, { 0 } };
static int fb_probe(device_t *d) {
    if (k.native) fmt(d->status, sizeof d->status, "linear framebuffer %ux%u, write-combining, QRT compositor", k.fb_w, k.fb_h);
    else strlcpy(d->status, "firmware GOP framebuffer, QRT compositor", sizeof d->status);
    return 0;
}
static const driver_t drv_fb = { "framebuffer", fb_pci, NULL, NULL, fb_probe, NULL };

/* ---- 16550 UART ------------------------------------------------------------ */
static const char *const uart_platform[] = { "com1", NULL };
static const char *const uart_acpi[] = { "PNP0501", NULL };
static int uart_vector = -1;
static void uart_status(device_t *d) {
    if (d->bus == BUS_ACPI) { strlcpy(d->status, "the same port as com1", sizeof d->status); return; }
    if (uart_vector >= 0)
        fmt(d->status, sizeof d->status, "COM1, IRQ 4 -> vector 0x%x, %llu bytes received", uart_vector, uart_rx_count());
    else fmt(d->status, sizeof d->status, "COM1, polled, %llu bytes received", uart_rx_count());
}
static int uart_probe(device_t *d) {
    if (!uart_present()) return DEV_NOT_MINE;
#if defined(__x86_64__)
    if (d->bus == BUS_PLATFORM && k.native && irq_ioapics()) {
        uart_vector = irq_attach_isa(4, "uart16550", uart_irq, NULL);
        if (uart_vector >= 0) uart_irq_enable();
    }
#endif
    uart_status(d);
    return 0;
}
static const driver_t drv_uart = { "uart16550", NULL, uart_acpi, uart_platform, uart_probe, uart_status };

/* ---- DesignWare I2C (Intel LPSS) ------------------------------------------ */
static const pci_match_t dw_pci[] = {
    { 0x8086, 0x0f41, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x0f42, PCI_ANY_CLS, PCI_ANY_CLS },
    { 0x8086, 0x0f43, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x0f44, PCI_ANY_CLS, PCI_ANY_CLS },
    { 0x8086, 0x0f45, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x0f46, PCI_ANY_CLS, PCI_ANY_CLS },
    { 0x8086, 0x0f47, PCI_ANY_CLS, PCI_ANY_CLS },
    { 0x8086, 0x22c1, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x22c2, PCI_ANY_CLS, PCI_ANY_CLS },
    { 0x8086, 0x22c3, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x22c4, PCI_ANY_CLS, PCI_ANY_CLS },
    { 0x8086, 0x22c5, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x22c6, PCI_ANY_CLS, PCI_ANY_CLS },
    { 0x8086, 0x22c7, PCI_ANY_CLS, PCI_ANY_CLS }, { 0 } };
static void dw_status(device_t *d) {
    int touch_bus = d->pci->bus == 0 && d->pci->dev == 0x18 && d->pci->fn == 6;
    strlcpy(d->status, touch_bus ? (ntouch_active() ? "I2C6: carries the touchscreen (native, polled)"
                                                    : "I2C6: touchscreen bus (Touch Lab)")
                                 : "controller known; no I2C client drivers on this bus yet", sizeof d->status);
}
static int dw_probe(device_t *d) { dw_status(d); return 0; }
static const driver_t drv_dwi2c = { "dw-i2c", dw_pci, NULL, NULL, dw_probe, dw_status };

/* ---- HID over I2C ------------------------------------------------------- */
static const char *const hid_acpi[] = { "PNP0C50", NULL };
static void hid_status(device_t *d) {
    strlcpy(d->status, ntouch_active() ? nt.status : "probed on demand by Touch Lab", sizeof d->status);
}
static int hid_probe(device_t *d) { hid_status(d); return 0; }
static const driver_t drv_i2chid = { "i2c-hid", NULL, hid_acpi, NULL, hid_probe, hid_status };

/* ---- hardware buttons (Venue 8 Pro 5855: GPIO pads, see buttons.c) ---------- */
static const char *const btn_acpi[] = { "ACPI0011", "INTCFD9", "PNP0C40", NULL };
static int btn_owner;                       /* several ACPI ids describe the same buttons */
static void btn_status(device_t *d) {
    if (d->priv) buttons_status(d->status, sizeof d->status);
    else strlcpy(d->status, "the same buttons as ACPI0011", sizeof d->status);
}
static int btn_probe(device_t *d) {
    if (!btn_owner) {
        if (!buttons_init()) return DEV_NOT_MINE;
        btn_owner = 1;
        d->priv = d;
    }
    btn_status(d);
    return 0;
}
static const driver_t drv_buttons = { "gpio-buttons", NULL, btn_acpi, NULL, btn_probe, btn_status };

/* ---- backlight (Venue 8 Pro 5855: LPSS PWM #1, see backlight.c) ------------------- */
static const char *const bl_acpi[] = { "80862288", NULL };
static void bl_status(device_t *d) { backlight_status(d->status, sizeof d->status); }
static int bl_probe(device_t *d) {
    if (!backlight_init()) return DEV_NOT_MINE;      /* software dimming: not this device's doing */
    bl_status(d);
    return 0;
}
static const driver_t drv_backlight = { "backlight", NULL, bl_acpi, NULL, bl_probe, bl_status };

/* ---- battery, charger, cover (Venue 8 Pro 5855: an embedded controller on I2C3, see battery.c) ---- */
static const char *const bat_acpi[] = { "PNP0C0A", NULL };
static void bat_status(device_t *d) { battery_status(d->status, sizeof d->status); }
static int bat_probe(device_t *d) { if (!k.is_venue) return DEV_NOT_MINE; bat_status(d); return 0; }
static const driver_t drv_battery = { "battery", NULL, bat_acpi, NULL, bat_probe, bat_status };

/* ---- the Integrated Sensor Hub (Venue 8 Pro 5855: accelerometer, light; see ish.c) ---- */
static const char *const ish_acpi[] = { "808622D8", NULL };
static const pci_match_t ish_pci[] = { { 0x8086, 0x22d8, PCI_ANY_CLS, PCI_ANY_CLS }, { 0 } };
static void ish_dev_status(device_t *d) { fmt(d->status, sizeof d->status, "sensor hub: %s", ish_status()); }
static int ish_dev_probe(device_t *d) {
    static int started;
    if (!k.is_venue) return DEV_NOT_MINE;
    if (!started) { started = 1; ish_start(); }
    ish_dev_status(d);
    return 0;
}
static const driver_t drv_ish = { "sensors", ish_pci, ish_acpi, NULL, ish_dev_probe, ish_dev_status };

/* ---- the PMIC on I2C7 (see pmic.c) ------------------------------------------------ */
static const char *const pmic_acpi[] = { "INT33FD", "INT33F5", "INT33F4", "INT34D3", NULL };
static int pmic_owner;
static void pmic_dev_status(device_t *d) { strlcpy(d->status, d->priv ? pmic_status() : "another description of the same PMIC", sizeof d->status); }
static int pmic_dev_probe(device_t *d) {
    if (!k.is_venue) return DEV_NOT_MINE;
    if (!pmic_owner) { pmic_owner = 1; d->priv = d; }
    pmic_dev_status(d);
    return 0;
}
static const driver_t drv_pmic = { "pmic", NULL, pmic_acpi, NULL, pmic_dev_probe, pmic_dev_status };

/* ---- Intel Wireless 8260 (src/drivers/iwm, started from the Wi-Fi app) ------------ */
static const pci_match_t iwm_pci[] = { { 0x8086, 0x24f3, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x24f4, PCI_ANY_CLS, PCI_ANY_CLS }, { 0 } };
static void iwm_dev_status(device_t *d) { strlcpy(d->status, iwm_status(), sizeof d->status); }
static int iwm_dev_probe(device_t *d) { if (!iwm_probe(d->pci)) return DEV_NOT_MINE; iwm_dev_status(d); return 0; }
static const driver_t drv_iwm = { "iwm (Wi-Fi)", iwm_pci, NULL, NULL, iwm_dev_probe, iwm_dev_status };

/* ---- Intel High Definition Audio (PCs, the Panasonic FZ-G1, QEMU's intel-hda) ----------- */
static const pci_match_t hda_pci[] = { { PCI_ANY_ID, PCI_ANY_ID, 0x04, 0x03 }, { 0 } };
static void hda_dev_status(device_t *d) { fmt(d->status, sizeof d->status, "HD Audio: %s", hda_status()); }
static int hda_dev_probe(device_t *d) {
    if (!k.native) { strlcpy(d->status, "the firmware's driver (if any) runs it", sizeof d->status); return 0; }
    hda_probe(d->pci);
    hda_dev_status(d);
    return 0;
}
static const driver_t drv_hda = { "hda", hda_pci, NULL, NULL, hda_dev_probe, hda_dev_status };

/* ---- Intel e1000/e1000e (QEMU's NIC; used to test the network stack) ------------- */
static const pci_match_t e1000_pci[] = { { 0x8086, 0x100e, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x100f, PCI_ANY_CLS, PCI_ANY_CLS },
                                         { 0x8086, 0x10d3, PCI_ANY_CLS, PCI_ANY_CLS }, { 0x8086, 0x153a, PCI_ANY_CLS, PCI_ANY_CLS }, { 0 } };
static void e1000_dev_status(device_t *d) { strlcpy(d->status, e1000_status(), sizeof d->status); }
static int e1000_dev_probe(device_t *d) {
    if (!e1000_probe(d->pci)) return DEV_NOT_MINE;
    if (k.native) e1000_start();
    e1000_dev_status(d);
    return 0;
}
static const driver_t drv_e1000 = { "e1000", e1000_pci, NULL, NULL, e1000_dev_probe, e1000_dev_status };

const driver_t *const builtin_drivers[] = { &drv_gpu, &drv_fb, &drv_uart, &drv_dwi2c, &drv_i2chid, &drv_buttons, &drv_backlight, &drv_battery, &drv_pmic, &drv_ish, &drv_iwm, &drv_e1000, &drv_xhci, &drv_hda, &drv_audio, &drv_chipset, NULL };
