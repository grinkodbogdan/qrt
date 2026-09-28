/* sysinfo.c - identify the machine: CPU, SMBIOS, ACPI, memory. */
#include "kernel.h"

static void cpuid(u32 leaf, u32 r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

static void probe_cpu(void) {
    u32 r[4];
    cpuid(0x80000000, r);
    if (r[0] >= 0x80000004) {
        u32 *out = (u32 *)k.cpu;
        for (u32 l = 0; l < 3; l++) {
            cpuid(0x80000002 + l, r);
            memcpy(out + l * 4, r, 16);
        }
        k.cpu[48] = 0;
        /* brand strings are often left-padded */
        char *s = k.cpu;
        while (*s == ' ') s++;
        memmove(k.cpu, s, strlen(s) + 1);
    } else {
        cpuid(0, r);
        memcpy(k.cpu, &r[1], 4); memcpy(k.cpu + 4, &r[3], 4); memcpy(k.cpu + 8, &r[2], 4);
        k.cpu[12] = 0;
    }
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) { return !memcmp(a, b, sizeof *a); }

static const char *smbios_str(const u8 *s, u8 idx) {
    if (!idx) return "";
    const char *p = (const char *)s + s[1];
    while (--idx && *p) p += strlen(p) + 1;
    return p;
}

static void parse_smbios(const u8 *t, usize len) {
    const u8 *end = t + len;
    while (t + 4 <= end && t[0] != 127) {
        if (t[1] < 4) break;
        if (t[0] == 0 && t[1] >= 6) {
            strlcpy(k.bios_version, smbios_str(t, t[5]), sizeof k.bios_version);
        } else if (t[0] == 1 && t[1] >= 8) {
            strlcpy(k.sys_vendor, smbios_str(t, t[4]), sizeof k.sys_vendor);
            strlcpy(k.sys_product, smbios_str(t, t[5]), sizeof k.sys_product);
        }
        /* skip formatted area, then the double-NUL-terminated string set */
        const u8 *p = t + t[1];
        while (p + 1 < end && (p[0] || p[1])) p++;
        t = p + 2;
    }
}

static void probe_tables(void) {
    EFI_GUID smb = SMBIOS_TABLE_GUID, smb3 = SMBIOS3_TABLE_GUID, acpi = ACPI20_TABLE_GUID;
    for (UINTN i = 0; i < k.st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *c = &k.st->ConfigurationTable[i];
        const u8 *p = c->VendorTable;
        if (guid_eq(&c->VendorGuid, &smb3) && !memcmp(p, "_SM3_", 5)) {
            u64 addr = *(const u64 *)(p + 0x10);
            u32 len = *(const u32 *)(p + 0x0c);
            if (addr < ((u64)1 << (sizeof(void *) * 8 - 1)))
                parse_smbios((const u8 *)(usize)addr, len);
        } else if (guid_eq(&c->VendorGuid, &smb) && !memcmp(p, "_SM_", 4) && !k.sys_product[0]) {
            u32 addr = *(const u32 *)(p + 0x18);
            u16 len = *(const u16 *)(p + 0x16);
            parse_smbios((const u8 *)(usize)addr, len);
        } else if (guid_eq(&c->VendorGuid, &acpi) && !memcmp(p, "RSD PTR ", 8)) {
            memcpy(k.acpi_oem, p + 9, 6);
            k.acpi_oem[6] = 0;
        }
    }
}

static void probe_memory(void) {
    UINTN sz = 0, key, dsz;
    u32 dver;
    k.bs->GetMemoryMap(&sz, NULL, &key, &dsz, &dver);
    sz += 16 * sizeof(EFI_MEMORY_DESCRIPTOR) + 1024;
    u8 *map = kalloc(sz);
    if (!EFI_ERROR(k.bs->GetMemoryMap(&sz, (EFI_MEMORY_DESCRIPTOR *)map, &key, &dsz, &dver))) {
        for (UINTN off = 0; off < sz; off += dsz) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + off);
            /* loader, boot services, runtime, conventional, ACPI reclaim/NVS */
            if ((d->Type >= 1 && d->Type <= 7) || d->Type == 9 || d->Type == 10)
                k.ram_bytes += d->NumberOfPages * 4096;
        }
    }
    kfree(map);
}

void sysinfo_probe(void) {
    probe_cpu();
    probe_tables();
    probe_memory();
    str16_to_utf8(k.fw_vendor, sizeof k.fw_vendor, k.st->FirmwareVendor);
    k.fw_revision = k.st->FirmwareRevision;
    k.uefi_revision = k.st->Hdr.Revision;
    k.is_venue = str_icontains(k.sys_product, "Venue");
    char ram[24];
    fmt_bytes(ram, sizeof ram, k.ram_bytes);
    klog("cpu: %s", k.cpu);
    klog("machine: %s %s", k.sys_vendor[0] ? k.sys_vendor : "?", k.sys_product[0] ? k.sys_product : "?");
    klog("firmware: %s rev %x, UEFI %u.%u (%s)", k.fw_vendor, k.fw_revision,
         k.uefi_revision >> 16, (k.uefi_revision & 0xffff) / 10, QRT_ARCH);
    klog("memory: %s usable", ram);
    if (k.is_venue) klog("Dell Venue detected - welcome home");
}
