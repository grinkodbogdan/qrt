/*
 * kernel.h - QRT kernel ("Tessera"): a firmware-hosted exokernel.
 *
 * Tessera never calls ExitBootServices().  The platform firmware keeps
 * running underneath it and acts as the driver layer: GOP for display,
 * Absolute/Simple Pointer for touch and mouse, Simple Text Input for keys
 * and hardware buttons, Block I/O + Simple FS for eMMC/SD/USB storage,
 * the RTC for wall time and NVRAM for persistent settings.  The kernel
 * owns everything above that: device discovery, a monotonic clock, input
 * normalisation, the event loop and the task model the shell runs on.
 */
#pragma once
#include "rt.h"

#define QRT_VERSION "0.3.0"
#define QRT_ARCH (sizeof(void *) == 8 ? "x86_64" : "ia32")

#define MAX_ABS 8
#define MAX_REL 8
#define MAX_VOL 16
#define MAX_BLK 32

typedef struct {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    char label[48];
    u64 size, free;
    int boot;          /* the volume QRT was loaded from */
    int read_only;
} volume_t;

typedef struct {
    u64 bytes;
    int removable, partition, read_only;
} blockdev_t;

typedef struct {
    EFI_SYSTEM_TABLE *st;
    EFI_BOOT_SERVICES *bs;
    EFI_RUNTIME_SERVICES *rt;
    EFI_HANDLE image;
    int graphics_up;

    /* display */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    u32 fb_w, fb_h;

    /* input */
    EFI_ABSOLUTE_POINTER_PROTOCOL *abs[MAX_ABS];
    int n_abs;
    EFI_SIMPLE_POINTER_PROTOCOL *rel[MAX_REL];
    int n_rel;
    int splitter_abs, splitter_rel;   /* fall back to ConIn's aggregate pointers */
    u32 touch_map;                     /* TOUCH_* bits, for panels mounted rotated */

    /* storage */
    volume_t vol[MAX_VOL];
    int n_vol;
    blockdev_t blk[MAX_BLK];
    int n_blk;

    /* platform facts */
    char cpu[64];
    char sys_vendor[48], sys_product[64], bios_version[48];
    char fw_vendor[64];
    u32 fw_revision, uefi_revision;
    char acpi_oem[8];
    const u8 *dsdt;                   /* AML of the DSDT (NULL if unreachable) */
    u32 dsdt_len;
    u64 ram_bytes;
    int is_venue;
    int handles, drivers_connected;

    /* clock */
    u64 tsc_per_ms;
    u64 tsc_boot;
    u64 boot_ms;       /* time from efi_main to first frame */
} kernel_t;

extern kernel_t k;

/* ---- clock ---- */
u64  k_now_ms(void);
u64  k_now_us(void);
void k_walltime(EFI_TIME *t);

/* ---- input: normalised event stream (physical framebuffer coords) ---- */
typedef enum { EV_NONE, EV_DOWN, EV_MOVE, EV_UP, EV_KEY, EV_SCROLL } ev_type_t;
typedef struct {
    ev_type_t type;
    int x, y;          /* EV_DOWN/MOVE/UP */
    int dy;            /* EV_SCROLL */
    u16 scan;          /* EV_KEY */
    c16 ch;            /* EV_KEY */
    int from_mouse;    /* pointer came from a relative device (draw a cursor) */
} event_t;

#define TOUCH_SWAP_XY 1
#define TOUCH_FLIP_X  2
#define TOUCH_FLIP_Y  4
void hal_set_touch_map(u32 map);

int  hal_poll(event_t *out, int max);
void hal_cursor(int *x, int *y, int *visible);

/* ---- power / firmware ---- */
void hal_shutdown(void);
void hal_reboot(void);
int  hal_reboot_to_firmware(void);   /* returns 0 if unsupported */

/* ---- persistent settings (UEFI NVRAM) ---- */
u32  hal_setting_get(const c16 *name, u32 def);
void hal_setting_set(const c16 *name, u32 value);

/* ---- boot-time probing ---- */
void hal_probe(void);
void sysinfo_probe(void);
int  hwreport_save(void);            /* ACPI/SMBIOS/PCI dump to \qrt\hwdump */
int  hwreport_write(const char *name, const void *data, usize len);
void hal_reprobe_input(void);        /* forget and rediscover firmware pointers */

/* ---- the shell (user interface) takes over after boot ---- */
void shell_main(void);
