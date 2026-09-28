/* irq.h - device interrupts on the native kernel.
 *
 * Legacy and ACPI interrupt lines are routed through the I/O APIC(s) listed
 * in the MADT (honouring its interrupt source overrides); PCI devices can use
 * MSI instead.  Either way the driver gets a vector from a shared pool and a
 * handler called with its own argument, after the local APIC has been
 * acknowledged.  Handlers run with interrupts off on the boot core: keep them
 * short (drain a FIFO, wake a thread). */
#pragma once
#include "../../kernel/kernel.h"

typedef void (*dev_irq_fn)(void *arg);

typedef struct {
    int vector;
    u32 gsi;                /* global system interrupt, or ~0u for MSI */
    const char *owner;
    volatile u64 count;
} irq_line_t;

int  irq_init(void);                                 /* parse MADT, mask every I/O APIC input; count of I/O APICs */
int  irq_attach_isa(int isa_irq, const char *owner, dev_irq_fn fn, void *arg);   /* returns vector or -1 */
int  irq_attach_gsi(u32 gsi, int level, int active_low, const char *owner, dev_irq_fn fn, void *arg);
int  irq_attach_msi(u8 bus, u8 dev, u8 fn, const char *owner, dev_irq_fn h, void *arg);
void irq_mask_gsi(u32 gsi, int masked);

int  irq_lines(const irq_line_t **out);              /* attached lines, for the System app */
int  irq_ioapics(void);
