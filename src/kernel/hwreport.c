/*
 * hwreport.c - dump the machine's hardware description to the boot volume.
 *
 * Native drivers (I2C touch, SDIO, audio, battery...) must be written
 * against this tablet's real ACPI namespace, not guesses.  On boot we save
 * every ACPI table (DSDT/SSDTs included), the raw SMBIOS table and a PCI
 * device list to \qrt\hwdump\ on the stick, plus report.txt with the ACPI
 * device IDs (_HID) found in the AML.  Decompile with `iasl -d *.aml`.
 */
#include "kernel.h"
#include "vfs.h"

static EFI_GUID li_guid = LOADED_IMAGE_GUID;
static EFI_GUID sfs_guid = SIMPLE_FS_GUID;
static EFI_GUID pci_guid = PCI_IO_GUID;

typedef struct { char sig[4]; u32 len; u8 rev, sum; char oem[6], oem_table[8]; u32 oem_rev, creator, creator_rev; } acpi_hdr_t;

static EFI_FILE_PROTOCOL *dir;
static char *report;
static usize rlen;
#define REPORT_CAP 65536

static void rpt(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    if (rlen < REPORT_CAP) rlen += vfmt(report + rlen, REPORT_CAP - rlen, f, ap);
    if (rlen > REPORT_CAP) rlen = REPORT_CAP;
    va_end(ap);
}

static int save(const char *name, const void *data, usize len) {
    c16 wname[64];
    EFI_FILE_PROTOCOL *f;
    utf8_to_str16(wname, 64, name);
    if (EFI_ERROR(dir->Open(dir, &f, wname, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)))
        return 0;
    UINTN n = len;
    EFI_STATUS s = f->Write(f, &n, (void *)data);
    f->Close(f);
    return !EFI_ERROR(s);
}

/* ---- ACPI --------------------------------------------------------------- */
static int n_hids;
static char hids[128][12];

static void add_hid(const char *id) {
    for (int i = 0; i < n_hids; i++) if (!strcmp(hids[i], id)) return;
    if (n_hids < 128) strlcpy(hids[n_hids++], id, sizeof hids[0]);
}

/* Heuristic AML scan: Name(_HID, "STRING") or Name(_HID, EISAID("PNP0C0A")). */
static void scan_hids(const u8 *aml, u32 len) {
    for (u32 i = 0; i + 10 < len; i++) {
        if (memcmp(aml + i, "_HID", 4) && memcmp(aml + i, "_CID", 4)) continue;
        const u8 *p = aml + i + 4;
        char id[12];
        if (p[0] == 0x0d) {                     /* StringPrefix */
            int n = 0;
            while (n < 10 && p[1 + n] >= 0x20 && p[1 + n] < 0x7f) { id[n] = (char)p[1 + n]; n++; }
            if (p[1 + n] == 0 && n >= 4) { id[n] = 0; add_hid(id); }
        } else if (p[0] == 0x0c) {              /* DWordPrefix: compressed EISA id */
            u32 v = p[1] | p[2] << 8 | p[3] << 16 | (u32)p[4] << 24;
            u32 be = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
            id[0] = (char)('@' + ((be >> 26) & 31));
            id[1] = (char)('@' + ((be >> 21) & 31));
            id[2] = (char)('@' + ((be >> 16) & 31));
            fmt(id + 3, 9, "%04X", be & 0xffff);
            if (id[0] > '@' && id[1] > '@' && id[2] > '@') add_hid(id);
        }
    }
}

static int table_count[64];
static char table_sig[64][5];
static int n_sigs;

static void dump_table(const acpi_hdr_t *t) {
    char sig[5], name[16];
    if (!t || t->len < sizeof *t || t->len > (16u << 20)) return;
    memcpy(sig, t->sig, 4);
    sig[4] = 0;
    for (int i = 0; i < 4; i++) if (sig[i] < 'A' || sig[i] > 'Z') if (sig[i] < '0' || sig[i] > '9') sig[i] = '_';
    int idx = 0, s;
    for (s = 0; s < n_sigs; s++) if (!strcmp(table_sig[s], sig)) break;
    if (s == n_sigs && n_sigs < 64) { strlcpy(table_sig[n_sigs], sig, 5); n_sigs++; }
    if (s < 64) idx = table_count[s]++;
    if (idx) fmt(name, sizeof name, "%s%d.aml", sig, idx);
    else fmt(name, sizeof name, "%s.aml", sig);
    char oem[7], oemt[9];
    memcpy(oem, t->oem, 6); oem[6] = 0;
    memcpy(oemt, t->oem_table, 8); oemt[8] = 0;
    int ok = save(name, t, t->len);
    rpt("  %-12s %6u bytes  OEM %-6s %-8s rev %u%s\r\n", name, t->len, oem, oemt, t->rev, ok ? "" : "  (write failed)");
    if (!memcmp(t->sig, "DSDT", 4) || !memcmp(t->sig, "SSDT", 4))
        scan_hids((const u8 *)t + sizeof *t, t->len - (u32)sizeof *t);
}

static const void *phys(u64 a) {
    if (sizeof(void *) == 4 && a >> 32) return NULL;
    return (const void *)(usize)a;
}

static void dump_acpi(const u8 *rsdp) {
    char oem[7];
    memcpy(oem, rsdp + 9, 6);
    oem[6] = 0;
    rpt("ACPI tables (RSDP rev %u, OEM %s)\r\n", rsdp[15], oem);
    const acpi_hdr_t *root = NULL;
    int wide = 0;
    if (rsdp[15] >= 2 && *(const u64 *)(rsdp + 24)) { root = phys(*(const u64 *)(rsdp + 24)); wide = 1; }
    if (!root) root = phys(*(const u32 *)(rsdp + 16));
    if (!root) { rpt("  no RSDT/XSDT reachable\r\n"); return; }
    dump_table(root);
    u32 n = (root->len - (u32)sizeof *root) / (wide ? 8 : 4);
    const u8 *ent = (const u8 *)root + sizeof *root;
    for (u32 i = 0; i < n; i++) {
        u64 a = wide ? *(const u64 *)(ent + i * 8) : *(const u32 *)(ent + i * 4);
        const acpi_hdr_t *t = phys(a);
        if (!t) continue;
        dump_table(t);
        if (!memcmp(t->sig, "FACP", 4) && t->len >= 148) {
            const u8 *f = (const u8 *)t;
            u64 dsdt = *(const u64 *)(f + 140);
            if (!dsdt) dsdt = *(const u32 *)(f + 40);
            dump_table(phys(dsdt));
        }
    }
    rpt("\r\nACPI device IDs found in DSDT/SSDT (_HID/_CID):\r\n ");
    for (int i = 0; i < n_hids; i++) rpt(" %s%s", hids[i], (i % 8 == 7) ? "\r\n " : "");
    rpt("\r\n\r\n");
}

/* ---- PCI ------------------------------------------------------------------ */
static void dump_pci(void) {
    UINTN n = 0;
    EFI_HANDLE *h = NULL;
    rpt("PCI devices (via firmware PCI I/O)\r\n");
    if (EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &n, &h))) { rpt("  none\r\n\r\n"); return; }
    for (UINTN i = 0; i < n; i++) {
        EFI_PCI_IO_PROTOCOL *p;
        u32 cfg[16];
        UINTN seg, bus, dev, fn;
        if (EFI_ERROR(k.bs->HandleProtocol(h[i], &pci_guid, (void **)&p))) continue;
        if (EFI_ERROR(p->Pci.Read(p, 2 /* uint32 */, 0, 16, cfg))) continue;
        p->GetLocation(p, &seg, &bus, &dev, &fn);
        rpt("  %02x:%02x.%x  %04x:%04x  class %02x.%02x.%02x\r\n", (u32)bus, (u32)dev, (u32)fn,
            cfg[0] & 0xffff, cfg[0] >> 16, cfg[2] >> 24, (cfg[2] >> 16) & 0xff, (cfg[2] >> 8) & 0xff);
    }
    k.bs->FreePool(h);
    rpt("\r\n");
}

/* ---- entry ------------------------------------------------------------------ */
/* Write one file into \qrt\hwdump on the boot volume (used by the Touch Lab). */
int hwreport_write(const char *name, const void *data, usize len) {
    if (k.native) {                        /* no storage driver yet: keep it in the RAM file system */
        char path[96];
        fmt(path, sizeof path, "/qrt/hwdump/%s", name);
        vnode_t *n = vfs_create(path, 0);
        vfs_truncate(n);
        return vfs_write(n, 0, data, len) == (i64)len;
    }
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *qrt;
    const u64 mode = EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE;
    if (EFI_ERROR(k.bs->HandleProtocol(k.image, &li_guid, (void **)&li)) ||
        EFI_ERROR(k.bs->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs)) ||
        EFI_ERROR(fs->OpenVolume(fs, &root)))
        return 0;
    int ok = 0;
    if (!EFI_ERROR(root->Open(root, &qrt, u"qrt", mode, EFI_FILE_DIRECTORY))) {
        if (!EFI_ERROR(qrt->Open(qrt, &dir, u"hwdump", mode, EFI_FILE_DIRECTORY))) {
            c16 wname[64];
            EFI_FILE_PROTOCOL *f;
            utf8_to_str16(wname, 64, name);
            /* truncate: delete any old copy first */
            if (!EFI_ERROR(dir->Open(dir, &f, wname, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) f->Delete(f);
            ok = save(name, data, len);
            dir->Close(dir);
        }
        qrt->Close(qrt);
    }
    root->Close(root);
    return ok;
}

int hwreport_save(void) {
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *qrt;
    if (EFI_ERROR(k.bs->HandleProtocol(k.image, &li_guid, (void **)&li)) ||
        EFI_ERROR(k.bs->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs)) ||
        EFI_ERROR(fs->OpenVolume(fs, &root)))
        return 0;
    const u64 mode = EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE;
    if (EFI_ERROR(root->Open(root, &qrt, u"qrt", mode, EFI_FILE_DIRECTORY))) { root->Close(root); return 0; }
    EFI_STATUS s = qrt->Open(qrt, &dir, u"hwdump", mode, EFI_FILE_DIRECTORY);
    qrt->Close(qrt);
    root->Close(root);
    if (EFI_ERROR(s)) return 0;

    report = kalloc(REPORT_CAP + 1);
    rlen = 0; n_hids = 0; n_sigs = 0;
    memset(table_count, 0, sizeof table_count);
    rpt("QRT %s hardware report (%s)\r\n\r\n", QRT_VERSION, QRT_ARCH);
    rpt("Machine   %s %s\r\nBIOS      %s\r\nCPU       %s\r\nFirmware  %s rev %x, UEFI %u.%u\r\n",
        k.sys_vendor, k.sys_product, k.bios_version, k.cpu, k.fw_vendor, k.fw_revision,
        k.uefi_revision >> 16, (k.uefi_revision & 0xffff) / 10);
    char ram[24];
    fmt_bytes(ram, sizeof ram, k.ram_bytes);
    rpt("RAM       %s\r\nDisplay   %ux%u (GOP)\r\nInput     %d touch, %d pointer\r\n\r\n",
        ram, k.fb_w, k.fb_h, k.n_abs, k.n_rel);

    EFI_GUID acpi20 = ACPI20_TABLE_GUID, acpi10 = ACPI10_TABLE_GUID, smb = SMBIOS_TABLE_GUID, smb3 = SMBIOS3_TABLE_GUID;
    const u8 *rsdp = NULL;
    for (UINTN i = 0; i < k.st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *c = &k.st->ConfigurationTable[i];
        const u8 *p = c->VendorTable;
        if (!memcmp(&c->VendorGuid, &acpi20, sizeof acpi20) || (!rsdp && !memcmp(&c->VendorGuid, &acpi10, sizeof acpi10)))
            rsdp = p;
        else if (!memcmp(&c->VendorGuid, &smb3, sizeof smb3) && !memcmp(p, "_SM3_", 5)) {
            const void *t = phys(*(const u64 *)(p + 0x10));
            if (t) save("smbios3.bin", t, *(const u32 *)(p + 0x0c));
        } else if (!memcmp(&c->VendorGuid, &smb, sizeof smb) && !memcmp(p, "_SM_", 4)) {
            save("smbios.bin", (const void *)(usize)*(const u32 *)(p + 0x18), *(const u16 *)(p + 0x16));
        }
    }
    if (rsdp) dump_acpi(rsdp);
    else rpt("no ACPI RSDP in the UEFI configuration table\r\n\r\n");
    dump_pci();
    rpt("Boot log\r\n");
    for (int i = 0; klog_line(i); i++) rpt("  %s\r\n", klog_line(i));

    int ok = save("report.txt", report, rlen);
    kfree(report);
    dir->Flush(dir);
    dir->Close(dir);
    klog("hwreport: %d ACPI device IDs, saved to \\qrt\\hwdump", n_hids);
    return ok;
}
