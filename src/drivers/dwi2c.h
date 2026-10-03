/* dwi2c.h - native driver for the Synopsys DesignWare I2C controller
 * (Intel LPSS I2C on Bay Trail / Cherry Trail, PCI 8086:22c1..22c7). */
#pragma once
#include "../kernel/kernel.h"

typedef struct dwi2c {
    volatile u8 *base;          /* BAR0 MMIO */
    EFI_HANDLE pci_handle;
    u32 pci_id;                 /* device << 16 | vendor */
    u32 comp_type, comp_ver, comp_param;
    u32 rx_depth, tx_depth;
    u32 last_abort;             /* IC_TX_ABRT_SOURCE of the last failed transfer */
    int found;
    u32 saved[11], saved_priv[2];
    int has_saved;
} dwi2c_t;

#define DW_OK        0
#define DW_ETIMEOUT -1
#define DW_EABORT   -2          /* NACK or arbitration loss; see last_abort */
#define DW_ENODEV   -3

/* Locate the controller at PCI bus/dev/fn through the firmware's PCI I/O. */
int  dwi2c_find(dwi2c_t *c, u32 bus, u32 dev, u32 fn);
/* Write wlen bytes, then (repeated start) read rlen bytes. Either may be 0. */
int  dwi2c_xfer(dwi2c_t *c, u8 addr, const u8 *w, int wlen, u8 *r, int rlen);
const char *dwi2c_strerror(int err);
void dwi2c_standard_mode(dwi2c_t *c);      /* 100 kHz; set the controller up if the firmware never did */
void dwi2c_save(dwi2c_t *c);                /* snapshot the firmware's configuration */
int  dwi2c_restore(dwi2c_t *c, u8 pci_fn);  /* re-apply it after ExitBootServices */
