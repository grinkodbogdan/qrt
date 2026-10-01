/* dev.h - QRT's driver model.
 *
 * Buses enumerate devices (PCI through ECAM, ACPI through the _HID/_CID ids
 * in the DSDT/SSDTs, plus fixed platform devices such as COM1).  Drivers
 * declare what they match (PCI vendor/device/class, ACPI ids, platform
 * names) and a probe function; dev_init() binds the first driver whose table
 * matches and whose probe accepts.  Every device, bound or not, stays in one
 * list, so the System app shows exactly what still lacks a driver.
 *
 * Porting a Linux driver means filling a driver_t with its id tables and
 * turning its probe() into ours; see docs/drivers.md. */
#pragma once
#include "kernel.h"
#include "../drivers/pci.h"

typedef enum { BUS_PCI, BUS_ACPI, BUS_PLATFORM } bus_kind_t;

#define PCI_ANY_ID 0xffff
#define PCI_ANY_CLS 0xff
typedef struct { u16 vendor, device; u8 class_code, subclass; } pci_match_t;   /* ends at vendor == 0 */

typedef struct device device_t;

typedef struct driver {
    const char *name;
    const pci_match_t *pci;           /* NULL: not a PCI driver */
    const char *const *acpi;          /* NULL-terminated ids, or NULL */
    const char *const *platform;      /* NULL-terminated names, or NULL */
    int (*probe)(device_t *d);        /* 0 = bound; DEV_NOT_MINE = keep looking; other < 0 = failed */
    void (*status)(device_t *d);      /* optional: refresh d->status (called from the UI thread) */
} driver_t;

extern const driver_t *const builtin_drivers[];   /* src/drivers/builtin.c, NULL-terminated */

#define DEV_NOT_MINE (-1)
#define DEV_FAILED   (-2)

struct device {
    bus_kind_t bus;
    char name[24];                    /* "00:18.6", "PNP0C50", "com1" */
    pci_dev_t *pci;                   /* BUS_PCI */
    const char *what;                 /* human description when known */
    const driver_t *drv;
    int failed;
    char status[96];                  /* set by the driver */
    void *priv;
};

#define DEV_MAX 256              /* the Venue lists 27 PCI devices and ~90 ACPI ids */
extern device_t devs[DEV_MAX];
extern int n_devs;

void dev_init(void);                  /* enumerate buses, bind drivers */
void dev_refresh(void);               /* let drivers update their status lines */
int  dev_bound(void);
