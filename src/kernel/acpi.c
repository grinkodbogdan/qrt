/* acpi.c - locate ACPI tables (RSDP -> XSDT/RSDT).  Valid before and after
 * ExitBootServices: the tables live in ACPI memory, which is never reused. */
#include "kernel.h"

static const u8 *rsdp;

static const u8 *phys_ptr(u64 a) {
    if (sizeof(void *) == 4 && a >> 32) return NULL;
    return (const u8 *)(usize)a;
}

void acpi_init(void) {
    EFI_GUID a20 = ACPI20_TABLE_GUID, a10 = ACPI10_TABLE_GUID;
    for (UINTN i = 0; i < k.st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *c = &k.st->ConfigurationTable[i];
        if (!memcmp(&c->VendorGuid, &a20, sizeof a20) || (!rsdp && !memcmp(&c->VendorGuid, &a10, sizeof a10)))
            rsdp = c->VendorTable;
    }
    if (rsdp && memcmp(rsdp, "RSD PTR ", 8)) rsdp = NULL;
}

const u8 *acpi_table(const char *sig, int index) {
    if (!rsdp) acpi_init();
    if (!rsdp) return NULL;
    int wide = rsdp[15] >= 2 && *(const u64 *)(rsdp + 24);
    const u8 *root = phys_ptr(wide ? *(const u64 *)(rsdp + 24) : *(const u32 *)(rsdp + 16));
    if (!root) return NULL;
    u32 n = (*(const u32 *)(root + 4) - 36) / (wide ? 8 : 4);
    for (u32 i = 0; i < n; i++) {
        const u8 *t = phys_ptr(wide ? *(const u64 *)(root + 36 + i * 8) : *(const u32 *)(root + 36 + i * 4));
        if (t && !memcmp(t, sig, 4) && index-- == 0) return t;
    }
    return NULL;
}

const u8 *acpi_rsdp(void) { if (!rsdp) acpi_init(); return rsdp; }
