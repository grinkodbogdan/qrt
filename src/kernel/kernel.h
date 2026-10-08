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

#define QRT_VERSION "0.15.0"
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
    int native;         /* 1 once the firmware's boot services are gone (x86-64) */
    u64 image_base, image_size;
    u64 epoch_at_boot;  /* Unix time (UTC) when k_now_ms() was 0; moves when the network sets the time */

    /* display */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    u32 fb_w, fb_h;
    u64 fb_base;        /* native mode: linear framebuffer */
    u32 fb_stride;      /* pixels per scan line */
    int fb_rgb;         /* 1 if the panel wants R,G,B byte order (else B,G,R) */

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
    const u8 *fadt;                   /* the FADT (NULL if unreachable) */
    u32 dsdt_len;
    u64 ram_bytes;
    int is_venue;
    char boot_note[80];               /* which kernel mode this boot runs in, and why */
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
void k_walltime(EFI_TIME *t);       /* local date and time */
/* the wall clock (time.c): UTC from the RTC, corrected by SNTP once the network is up */
void time_init(void);
u64  time_utc(void);
u64  time_utc_us(void);
void time_set_utc(u64 utc, const char *source);
int  time_synced(void);
u64  time_synced_ago_ms(void);
int  time_zone(void);               /* local = UTC + this many seconds */
int  time_zone_known(void);
void time_set_zone(int offset_s);

/* ---- input: normalised event stream (physical framebuffer coords) ---- */
/* EV_REL: a mouse report - x, y movement (or, with from_mouse == 2, an absolute
 * position 0..65535 on each axis), dy the wheel, scan the buttons (bit 0 left,
 * 1 right, 2 middle).  The shell turns it into cursor moves and clicks. */
typedef enum { EV_NONE, EV_DOWN, EV_MOVE, EV_UP, EV_KEY, EV_SCROLL, EV_REL } ev_type_t;
typedef struct {
    ev_type_t type;
    int x, y;          /* EV_DOWN/MOVE/UP */
    int dy;            /* EV_SCROLL */
    u16 scan;          /* EV_KEY */
    c16 ch;            /* EV_KEY */
    int from_mouse;    /* pointer came from a relative device (draw a cursor) */
    int fingers;       /* EV_DOWN/MOVE/UP from a touchscreen: contacts down now (0: unknown, 1 finger) */
} event_t;

#define TOUCH_SWAP_XY 1
#define TOUCH_FLIP_X  2
#define TOUCH_FLIP_Y  4
void hal_set_touch_map(u32 map);

int  hal_poll(event_t *out, int max);
void hal_cursor(int *x, int *y, int *visible);

/* ---- platform services with a firmware and a native implementation ---- */
void hal_present(const u32 *px, int stride, int x, int y, int w, int h);  /* rect -> panel */
void hal_wait_frame(void);           /* sleep until the next ~10 ms frame tick */
void hal_wait_frame_ms(u32 ms);     /* native: sleep ms (other threads run); firmware: next tick */
void hal_delay_us(u32 us);
void *hal_dma_alloc(usize bytes);        /* zeroed, page-aligned, contiguous; pointer == bus address */
void hal_settings_prepare(void);     /* cache + make settings reachable after ExitBootServices */
const char *hal_mode(void);          /* "firmware-hosted" or "native" */

/* ---- power / firmware ---- */
void hal_shutdown(void);
void power_off(void);                /* power.c */
void install_if_asked(void);         /* install.c: at boot, when Settings asked for QRT on the internal disk */
void install_probe(void);            /* at boot: which disk an install would erase */
u64  install_target_bytes(void);     /* its size (0: nothing to install on) */
void power_restart(void);
void hal_reboot(void);
int  hal_reboot_to_firmware(void);   /* returns 0 if unsupported */

/* ---- persistent settings (UEFI NVRAM) ---- */
u32  hal_setting_get(const c16 *name, u32 def);
void hal_setting_set(const c16 *name, u32 value);
usize hal_setting_get_blob(const c16 *name, void *buf, usize cap);
void hal_setting_set_blob(const c16 *name, const void *buf, usize len);

/* ---- boot-time probing ---- */
void hal_probe(void);
void sysinfo_probe(void);
int  hwreport_save(void);            /* ACPI/SMBIOS/PCI dump to \qrt\hwdump */
int  hwreport_write(const char *name, const void *data, usize len);
const char *hwreport_hid(int i);     /* i-th ACPI _HID/_CID found in the AML, NULL past the end */
void hal_reprobe_input(void);        /* forget and rediscover firmware pointers */

/* ---- ACPI ---- */
const u8 *acpi_table(const char *sig, int index);   /* n-th table with this signature */

/* ---- the shell (user interface) takes over after boot ---- */
void shell_main(void);
