/* acpidev.h - ACPI on PCs (ACPICA, src/acpi): battery, AC adapter, lid, buttons and
 * hotkeys, as Linux's ACPI drivers provide them.  Native kernel, non-Venue machines. */
#pragma once
#include "../kernel/kernel.h"
#include "../drivers/battery.h"

void acpi_start(void);                              /* after dev_init: the ACPI thread */
int  acpi_poll(event_t *out, int max);              /* key events (hal_poll) */
int  acpi_battery_fill(battery_t *b, char *status, usize cap);   /* 1 if ACPI has a battery or AC adapter */
const char *acpi_status(void);
