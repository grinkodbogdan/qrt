/*
 * install.c - putting QRT on the machine's internal disk.
 *
 * Settings asks for it (QrtInstall = 1) and restarts; on that boot, while the firmware's
 * disk access (Block I/O) still works, this copies the QRT disk QRT started from - its
 * GPT and its one FAT partition - onto the largest fixed disk that is not that one,
 * fixes the GPT for the bigger disk (the backup header and entries at its end), and
 * adds a "QRT" boot entry first in BootOrder.  What was on the disk (Windows) is gone:
 * Settings says so before it asks.  The request is cleared before anything is written,
 * so a failed install never repeats by itself.
 */
#include "kernel.h"

static EFI_GUID blk_guid = BLOCK_IO_GUID;
static EFI_GUID li_guid = LOADED_IMAGE_GUID;
static EFI_GUID dp_guid = {0x09576e91,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID global_guid = GLOBAL_VARIABLE_GUID;

typedef EFI_STATUS (*blk_rw_t)(EFI_BLOCK_IO_PROTOCOL *, u32, u64, UINTN, void *);
typedef EFI_STATUS (*blk_flush_t)(EFI_BLOCK_IO_PROTOCOL *);

#define VAR_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

/* ---- the screen: the firmware's text console ---------------------------------------------- */
static int quiet;                                   /* install_probe: find the disks, say nothing */
static u64 target_bytes;

static void say(const char *f, ...) {
    if (quiet) return;
    char line[160];
    c16 w[170];
    va_list ap;
    va_start(ap, f);
    vfmt(line, sizeof line, f, ap);
    va_end(ap);
    klog("install: %s", line);
    utf8_to_str16(w, 160, line);
    usize n = str16len(w);
    w[n] = '\r'; w[n + 1] = '\n'; w[n + 2] = 0;
    k.st->ConOut->OutputString(k.st->ConOut, w);
}

/* ---- device paths ------------------------------------------------------------------------- */
static usize dp_len(const u8 *dp) {                 /* bytes before the end node */
    usize n = 0;
    while (!(dp[n] == 0x7F && dp[n + 1] == 0xFF)) {
        u16 l = (u16)(dp[n + 2] | dp[n + 3] << 8);
        if (l < 4) break;
        n += l;
    }
    return n;
}

/* ---- CRC-32 (GPT headers and entries) ----------------------------------------------------- */
static u32 crc32(const u8 *p, usize n) {
    u32 c = 0xFFFFFFFFu;
    for (usize i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (u32)-(i32)(c & 1));
    }
    return ~c;
}

typedef struct {
    EFI_HANDLE h;
    EFI_BLOCK_IO_PROTOCOL *b;
    const u8 *dp;
} disk_t;

static void *aligned(usize bytes, u32 align, void **raw) {
    if (align < 16) align = 16;
    u8 *p = NULL;
    if (EFI_ERROR(k.bs->AllocatePool(EfiLoaderData, bytes + align, (void **)&p)) || !p) return NULL;
    *raw = p;
    return (void *)(((usize)p + align - 1) & ~(usize)(align - 1));
}

static int rd(disk_t *d, u64 lba, usize bytes, void *buf) {
    return !EFI_ERROR(((blk_rw_t)d->b->ReadBlocks)(d->b, d->b->Media->MediaId, lba, bytes, buf));
}
static int wr(disk_t *d, u64 lba, usize bytes, void *buf) {
    return !EFI_ERROR(((blk_rw_t)d->b->WriteBlocks)(d->b, d->b->Media->MediaId, lba, bytes, buf));
}

/* the QRT disk and the target */
static int find_disks(disk_t *src, disk_t *dst) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    const u8 *part_dp = NULL;
    if (EFI_ERROR(k.bs->HandleProtocol(k.image, &li_guid, (void **)&li)) || !li ||
        EFI_ERROR(k.bs->HandleProtocol(li->DeviceHandle, &dp_guid, (void **)&part_dp)) || !part_dp) {
        say("Cannot tell which disk QRT started from.");
        return 0;
    }
    usize part_len = dp_len(part_dp);
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &blk_guid, NULL, &n, &hs))) { say("No disks."); return 0; }
    memset(src, 0, sizeof *src);
    memset(dst, 0, sizeof *dst);
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *b;
        const u8 *dp = NULL;
        if (EFI_ERROR(k.bs->HandleProtocol(hs[i], &blk_guid, (void **)&b))) continue;
        if (!b->Media->MediaPresent || b->Media->LogicalPartition) continue;   /* whole disks only */
        k.bs->HandleProtocol(hs[i], &dp_guid, (void **)&dp);
        usize l = dp ? dp_len(dp) : 0;
        u64 bytes = (b->Media->LastBlock + 1) * b->Media->BlockSize;
        if (dp && l && l < part_len && !memcmp(dp, part_dp, l)) {           /* the partition QRT came from is on it */
            src->h = hs[i]; src->b = b; src->dp = dp;
            continue;
        }
        if (b->Media->RemovableMedia || b->Media->ReadOnly) continue;
        if (!dst->b || bytes > (dst->b->Media->LastBlock + 1) * dst->b->Media->BlockSize) { dst->h = hs[i]; dst->b = b; dst->dp = dp; }
    }
    k.bs->FreePool(hs);
    if (!src->b) { say("Cannot find the disk QRT started from."); return 0; }
    if (!dst->b) { say("No internal disk found to install on."); return 0; }
    return 1;
}

/* ---- the boot entry --------------------------------------------------------------------- */
static void put16le(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32le(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void put64le(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }

static int add_boot_entry(const u8 *entry, u32 part_no) {
    /* EFI_LOAD_OPTION: attributes, path list length, "QRT", HD(...)/File(\EFI\BOOT\BOOTX64.EFI)/End */
    static const char path[] = "\\EFI\\BOOT\\BOOTX64.EFI";
    u8 opt[256];
    usize o = 0;
    put32le(opt, 1);                                         /* LOAD_OPTION_ACTIVE */
    o = 6;
    const char *desc = "QRT";
    for (const char *c = desc; ; c++) { put16le(opt + o, (u16)*c); o += 2; if (!*c) break; }
    usize list = o;
    opt[o] = 4; opt[o + 1] = 1; put16le(opt + o + 2, 42);   /* media / hard drive */
    put32le(opt + o + 4, part_no);
    put64le(opt + o + 8, *(const u64 *)(entry + 32));        /* starting LBA */
    put64le(opt + o + 16, *(const u64 *)(entry + 40) - *(const u64 *)(entry + 32) + 1);
    memcpy(opt + o + 24, entry + 16, 16);                    /* the partition's unique GUID */
    opt[o + 40] = 2; opt[o + 41] = 2;                        /* GPT, GUID signature */
    o += 42;
    usize fl = 4 + 2 * sizeof path;
    opt[o] = 4; opt[o + 1] = 4; put16le(opt + o + 2, (u16)fl);   /* media / file path */
    for (usize i = 0; i < sizeof path; i++) put16le(opt + o + 4 + 2 * i, (u16)path[i]);
    o += fl;
    opt[o] = 0x7F; opt[o + 1] = 0xFF; put16le(opt + o + 2, 4); o += 4;
    put16le(opt + 4, (u16)(o - list));

    /* a free Boot#### */
    c16 name[9] = u"Boot0000";
    static const char hex[] = "0123456789ABCDEF";
    u16 num = 0;
    for (num = 0x1000; num < 0x2000; num++) {
        for (int i = 0; i < 4; i++) name[4 + i] = (c16)hex[(num >> (12 - 4 * i)) & 15];
        u8 tmp[8]; UINTN sz = sizeof tmp;
        EFI_STATUS st = k.rt->GetVariable(name, &global_guid, NULL, &sz, tmp);
        if (st == EFI_NOT_FOUND) break;
    }
    if (EFI_ERROR(k.rt->SetVariable(name, &global_guid, VAR_ATTR, o, opt))) { say("Could not add the boot entry."); return 0; }
    /* first in BootOrder */
    u16 order[128];
    UINTN sz = sizeof order - 2;
    if (EFI_ERROR(k.rt->GetVariable(u"BootOrder", &global_guid, NULL, &sz, order + 1))) sz = 0;
    usize cnt = sz / 2, out = 1;
    order[0] = num;
    for (usize i = 0; i < cnt; i++) if (order[1 + i] != num) order[out++] = order[1 + i];
    if (EFI_ERROR(k.rt->SetVariable(u"BootOrder", &global_guid, VAR_ATTR, out * 2, order))) say("Could not put QRT first in the boot order.");
    say("Boot entry Boot%c%c%c%c \"QRT\" added, first in the boot order.", hex[num >> 12], hex[(num >> 8) & 15], hex[(num >> 4) & 15], hex[num & 15]);
    return 1;
}

/* ---- the copy ----------------------------------------------------------------------------- */
static int install(void) {
    disk_t src, dst;
    if (!find_disks(&src, &dst)) return 0;
    u32 bs = src.b->Media->BlockSize;
    char a[24], b[24];
    fmt_bytes(a, sizeof a, (src.b->Media->LastBlock + 1) * bs);
    fmt_bytes(b, sizeof b, (dst.b->Media->LastBlock + 1) * dst.b->Media->BlockSize);
    say("From: the QRT disk (%s).  To: the internal disk (%s).", a, b);
    if (dst.b->Media->BlockSize != bs) { say("The disks' block sizes differ (%u, %u).", bs, dst.b->Media->BlockSize); return 0; }

    u32 align = MAX(src.b->Media->IoAlign, dst.b->Media->IoAlign);
    void *raw;
    usize chunk = 1 << 20;
    u8 *buf = aligned(chunk, align, &raw);
    if (!buf) { say("Out of memory."); return 0; }

    /* the QRT disk's GPT: how far its partitions go */
    if (!rd(&src, 1, bs, buf) || memcmp(buf, "EFI PART", 8)) { say("The QRT disk has no GPT."); return 0; }
    u8 hdr[512];
    memcpy(hdr, buf, MIN((usize)bs, sizeof hdr));
    u32 hsize = *(u32 *)(hdr + 12), nent = *(u32 *)(hdr + 80), esize = *(u32 *)(hdr + 84);
    u64 elba = *(u64 *)(hdr + 72);
    usize ebytes = (usize)nent * esize, eblocks = (ebytes + bs - 1) / bs;
    u8 *ents_raw_p; void *ents_raw;
    ents_raw_p = aligned(eblocks * bs, align, &ents_raw);
    if (!ents_raw_p || !rd(&src, elba, eblocks * bs, ents_raw_p)) { say("Cannot read the partition entries."); return 0; }
    u64 end = 0;
    int first = -1;
    for (u32 i = 0; i < nent; i++) {
        const u8 *e = ents_raw_p + (usize)i * esize;
        u64 last = *(const u64 *)(e + 40);
        int used = 0;
        for (int j = 0; j < 16; j++) used |= e[j];
        if (!used) continue;
        if (first < 0) first = (int)i;
        if (last > end) end = last;
    }
    u64 dlast = dst.b->Media->LastBlock;
    if (first < 0 || end + 1 + eblocks + 1 > dlast) { say("The internal disk is too small for QRT."); return 0; }

    /* copy LBA 0 .. end */
    u64 total = end + 1, per = chunk / bs, done = 0;
    int last_pct = -1;
    while (done < total) {
        u64 n = MIN(per, total - done);
        if (!rd(&src, done, n * bs, buf)) { say("Reading the QRT disk failed at block %llu.", done); return 0; }
        if (!wr(&dst, done, n * bs, buf)) { say("Writing the internal disk failed at block %llu.", done); return 0; }
        done += n;
        int pct = (int)(done * 100 / total);
        if (pct / 10 != last_pct / 10) { say("Copying: %d%%", pct); last_pct = pct; }
    }

    /* the installed disk and its partition get GUIDs of their own: the boot entry finds its
     * partition by GUID, and a copy of the stick's would let the firmware pick the stick */
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u64 seed = (((u64)hi << 32) | lo) ^ ((u64)(usize)buf << 17);
    for (int pass = 0; pass < 2; pass++) {
        u8 *g = pass ? hdr + 56 : ents_raw_p + (usize)first * esize + 16;   /* DiskGUID, UniquePartitionGUID */
        for (int i = 0; i < 16; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; g[i] = (u8)(seed >> 56); }
        g[7] = (u8)((g[7] & 0x0F) | 0x40);                   /* version 4 (random) */
        g[8] = (u8)((g[8] & 0x3F) | 0x80);
    }
    *(u32 *)(hdr + 88) = crc32(ents_raw_p, ebytes);          /* PartitionEntryArrayCRC32 */
    if (!wr(&dst, elba, eblocks * bs, ents_raw_p)) { say("Writing the partition entries failed."); return 0; }

    /* the GPT for this disk: primary header's backup and last usable block, then the backup copy */
    u64 bent = dlast - eblocks;
    memset(buf, 0, bs);
    memcpy(buf, hdr, hsize);
    *(u64 *)(buf + 32) = dlast;                              /* AlternateLBA */
    *(u64 *)(buf + 48) = bent - 1;                           /* LastUsableLBA */
    *(u32 *)(buf + 16) = 0;
    *(u32 *)(buf + 16) = crc32(buf, hsize);
    if (!wr(&dst, 1, bs, buf)) { say("Writing the GPT failed."); return 0; }
    if (!wr(&dst, bent, eblocks * bs, ents_raw_p)) { say("Writing the backup partition entries failed."); return 0; }
    *(u64 *)(buf + 24) = dlast;                              /* MyLBA */
    *(u64 *)(buf + 32) = 1;                                  /* AlternateLBA */
    *(u64 *)(buf + 72) = bent;                               /* PartitionEntryLBA */
    *(u32 *)(buf + 16) = 0;
    *(u32 *)(buf + 16) = crc32(buf, hsize);
    if (!wr(&dst, dlast, bs, buf)) { say("Writing the backup GPT failed."); return 0; }
    /* the protective MBR covers the whole disk */
    if (rd(&dst, 0, bs, buf)) {
        *(u32 *)(buf + 0x1BE + 12) = dlast > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)dlast;
        wr(&dst, 0, bs, buf);
    }
    if (dst.b->FlushBlocks) ((blk_flush_t)dst.b->FlushBlocks)(dst.b);
    say("QRT is on the internal disk.");

    add_boot_entry(ents_raw_p + (usize)first * esize, (u32)first + 1);
    k.bs->FreePool(raw);
    k.bs->FreePool(ents_raw);
    return 1;
}

/* At boot: the disk an install would erase, for Settings to name (0: none). */
void install_probe(void) {
    disk_t src, dst;
    quiet = 1;
    if (find_disks(&src, &dst)) target_bytes = (dst.b->Media->LastBlock + 1) * dst.b->Media->BlockSize;
    quiet = 0;
    char a[24], b[24];
    fmt_bytes(a, sizeof a, src.b ? (src.b->Media->LastBlock + 1) * src.b->Media->BlockSize : 0);
    fmt_bytes(b, sizeof b, target_bytes);
    klog("install: started from a %s disk; installing would erase a %s disk", a, b);
    u16 cur = 0xFFFF, order[16];
    UINTN sz = sizeof cur;
    k.rt->GetVariable(u"BootCurrent", &global_guid, NULL, &sz, &cur);
    sz = sizeof order;
    if (EFI_ERROR(k.rt->GetVariable(u"BootOrder", &global_guid, NULL, &sz, order))) sz = 0;
    char ord[80] = "";
    usize o = 0;
    for (usize i = 0; i < sz / 2 && o < sizeof ord - 6; i++) o += fmt(ord + o, sizeof ord - o, "%s%04x", i ? "," : "", order[i]);
    klog("install: firmware booted Boot%04x; boot order %s", cur, ord);
}
u64 install_target_bytes(void) { return target_bytes; }

/* At boot, before anything else needs the disks: install if Settings asked for it. */
void install_if_asked(void) {
    if (hal_setting_get(u"QrtInstall", 0) != 1) return;
    hal_setting_set(u"QrtInstall", 0);                       /* never twice by itself */
    k.st->ConOut->SetAttribute(k.st->ConOut, 0x0F);
    k.st->ConOut->ClearScreen(k.st->ConOut);
    say("Installing QRT %s on this machine's internal disk.", QRT_VERSION);
    say("Do not turn the machine off.");
    int ok = install();
    say(ok ? "Done. Remove the USB stick; restarting in 10 seconds." : "Not installed. Restarting in 20 seconds.");
    k.bs->Stall(ok ? 10000000 : 20000000);
    k.rt->ResetSystem(EfiResetCold, 0, 0, NULL);
}
