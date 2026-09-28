/*
 * hal.c - device discovery and the firmware-backed driver layer.
 */
#include "kernel.h"

static EFI_GUID gop_guid = GOP_GUID;
static EFI_GUID abs_guid = ABS_POINTER_GUID;
static EFI_GUID rel_guid = SIMPLE_POINTER_GUID;
static EFI_GUID sfs_guid = SIMPLE_FS_GUID;
static EFI_GUID blk_guid = BLOCK_IO_GUID;
static EFI_GUID fsinfo_guid = FS_INFO_GUID;
static EFI_GUID li_guid = LOADED_IMAGE_GUID;
static EFI_GUID global_guid = GLOBAL_VARIABLE_GUID;
static EFI_GUID qrt_guid = {0x51525400,0x7465,0x7373,{0x65,0x72,0x61,0x00,0x51,0x52,0x54,0x31}};

/* ---- display ------------------------------------------------------------ */
static void probe_display(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(k.bs->HandleProtocol(k.st->ConsoleOutHandle, &gop_guid, (void **)&gop)) || !gop)
        if (EFI_ERROR(k.bs->LocateProtocol(&gop_guid, NULL, (void **)&gop)))
            gop = NULL;
    if (!gop) return;

    /*
     * Keep the mode the firmware chose (on tablets that is the panel's
     * native resolution).  Only if it is tiny do we look for a larger one.
     */
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    if (info->HorizontalResolution < 640 || info->VerticalResolution < 480) {
        u32 best = gop->Mode->Mode;
        u64 best_px = 0;
        for (u32 m = 0; m < gop->Mode->MaxMode; m++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
            UINTN sz;
            if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &mi))) continue;
            u64 px = (u64)mi->HorizontalResolution * mi->VerticalResolution;
            if (px > best_px && mi->HorizontalResolution <= 1920 && mi->VerticalResolution <= 1920) {
                best_px = px; best = m;
            }
        }
        gop->SetMode(gop, best);
    }
    k.gop = gop;
    k.fb_w = gop->Mode->Info->HorizontalResolution;
    k.fb_h = gop->Mode->Info->VerticalResolution;
    klog("display: GOP %ux%u, mode %u of %u", k.fb_w, k.fb_h, gop->Mode->Mode, gop->Mode->MaxMode);
}

/* ---- input ------------------------------------------------------------- */
static void probe_input(void) {
    UINTN n = 0;
    EFI_HANDLE *h = NULL;

    /* Skip the console splitter's virtual devices: they aggregate the real
     * ones and would deliver every event twice. */
    if (!EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &abs_guid, NULL, &n, &h))) {
        for (UINTN i = 0; i < n && k.n_abs < MAX_ABS; i++) {
            EFI_ABSOLUTE_POINTER_PROTOCOL *p;
            if (h[i] == k.st->ConsoleInHandle) continue;
            if (EFI_ERROR(k.bs->HandleProtocol(h[i], &abs_guid, (void **)&p))) continue;
            p->Reset(p, 0);
            k.abs[k.n_abs++] = p;
            klog("input: absolute pointer (touch) %u..%u x %u..%u",
                 (u32)p->Mode->AbsoluteMinX, (u32)p->Mode->AbsoluteMaxX,
                 (u32)p->Mode->AbsoluteMinY, (u32)p->Mode->AbsoluteMaxY);
        }
        k.bs->FreePool(h);
    }
    if (!EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &rel_guid, NULL, &n, &h))) {
        for (UINTN i = 0; i < n && k.n_rel < MAX_REL; i++) {
            EFI_SIMPLE_POINTER_PROTOCOL *p;
            if (h[i] == k.st->ConsoleInHandle) continue;
            if (EFI_ERROR(k.bs->HandleProtocol(h[i], &rel_guid, (void **)&p))) continue;
            p->Reset(p, 0);
            k.rel[k.n_rel++] = p;
            klog("input: relative pointer (mouse/trackpad), %u/%u counts per mm",
                 (u32)p->Mode->ResolutionX, (u32)p->Mode->ResolutionY);
        }
        k.bs->FreePool(h);
    }
    /* The console splitter's virtual pointers are only useful when no
     * physical device was found (some firmware hides them behind it); they
     * are polled but not counted as hardware. */
    if (!k.n_abs && !EFI_ERROR(k.bs->HandleProtocol(k.st->ConsoleInHandle, &abs_guid, (void **)&k.abs[0])))
        k.splitter_abs = 1;
    if (!k.n_rel && !EFI_ERROR(k.bs->HandleProtocol(k.st->ConsoleInHandle, &rel_guid, (void **)&k.rel[0])))
        k.splitter_rel = 1;
    klog("input: %d touch, %d pointer, console keys/buttons", k.n_abs, k.n_rel);
}

static int abs_down[MAX_ABS];
static int abs_last_x[MAX_ABS], abs_last_y[MAX_ABS];
static int cur_x = -1, cur_y = -1, cur_visible, rel_down;

void hal_cursor(int *x, int *y, int *visible) { *x = cur_x; *y = cur_y; *visible = cur_visible; }

static int scale_axis(u64 v, u64 lo, u64 hi, u32 size) {
    if (hi <= lo) return (int)MIN(v, (u64)size - 1);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)((v - lo) * (size - 1) / (hi - lo));
}

/*
 * Touch injection over the console, for automated testing in emulators
 * whose firmware has no pointer drivers (stock OVMF).  A packet is
 *   Ctrl-T  <d|m|u>  <x> , <y>  ;
 * in physical pixels and becomes EV_DOWN / EV_MOVE / EV_UP exactly as if a
 * touchscreen had produced it.  Returns 1 while consuming a packet.
 */
static int inject_key(c16 ch, event_t *out) {
    static int active, len;
    static char buf[24];
    memset(out, 0, sizeof *out);
    if (ch == 0x14) { active = 1; len = 0; return 1; }
    if (!active) return 0;
    if (ch != ';') {
        if (len < (int)sizeof buf - 1 && ch < 128) buf[len++] = (char)ch;
        else active = 0;
        return 1;
    }
    active = 0;
    buf[len] = 0;
    int x = 0, y = 0, *v = &x;
    for (char *p = buf + 1; *p; p++) {
        if (*p == ',') v = &y;
        else if (*p >= '0' && *p <= '9') *v = *v * 10 + (*p - '0');
    }
    out->x = CLAMP(x, 0, (int)k.fb_w - 1);
    out->y = CLAMP(y, 0, (int)k.fb_h - 1);
    out->type = buf[0] == 'd' ? EV_DOWN : buf[0] == 'u' ? EV_UP : EV_MOVE;
    return 1;
}

int hal_poll(event_t *out, int max) {
    int n = 0;
    if (cur_x < 0) { cur_x = (int)k.fb_w / 2; cur_y = (int)k.fb_h / 2; }

    int n_abs = k.n_abs ? k.n_abs : k.splitter_abs, n_rel = k.n_rel ? k.n_rel : k.splitter_rel;
    for (int i = 0; i < n_abs && n < max - 1; i++) {
        EFI_ABSOLUTE_POINTER_PROTOCOL *p = k.abs[i];
        EFI_ABSOLUTE_POINTER_STATE s;
        if (EFI_ERROR(p->GetState(p, &s))) continue;
        EFI_ABSOLUTE_POINTER_MODE *m = p->Mode;
        u64 ax = s.CurrentX, ay = s.CurrentY;
        u64 ax0 = m->AbsoluteMinX, ax1 = m->AbsoluteMaxX, ay0 = m->AbsoluteMinY, ay1 = m->AbsoluteMaxY;
        if (k.touch_map & TOUCH_SWAP_XY) {
            u64 t = ax; ax = ay; ay = t;
            t = ax0; ax0 = ay0; ay0 = t;
            t = ax1; ax1 = ay1; ay1 = t;
        }
        int x = scale_axis(ax, ax0, ax1, k.fb_w);
        int y = scale_axis(ay, ay0, ay1, k.fb_h);
        if (k.touch_map & TOUCH_FLIP_X) x = (int)k.fb_w - 1 - x;
        if (k.touch_map & TOUCH_FLIP_Y) y = (int)k.fb_h - 1 - y;
        int down = s.ActiveButtons & 1;
        event_t e = { .x = x, .y = y };
        if (down && !abs_down[i]) e.type = EV_DOWN;
        else if (!down && abs_down[i]) e.type = EV_UP;
        else if (x != abs_last_x[i] || y != abs_last_y[i]) e.type = EV_MOVE;
        abs_down[i] = down; abs_last_x[i] = x; abs_last_y[i] = y;
        cur_visible = 0;
        if (e.type) out[n++] = e;
    }

    for (int i = 0; i < n_rel && n < max - 2; i++) {
        EFI_SIMPLE_POINTER_PROTOCOL *p = k.rel[i];
        EFI_SIMPLE_POINTER_STATE s;
        if (EFI_ERROR(p->GetState(p, &s))) continue;
        /* Resolution is counts/mm; aim for roughly 4 px per mm of travel. */
        i64 rx = p->Mode->ResolutionX ? (i64)p->Mode->ResolutionX : 1;
        i64 ry = p->Mode->ResolutionY ? (i64)p->Mode->ResolutionY : 1;
        /* keep the sub-pixel remainder so slow movements are not lost */
        static i64 acc_x, acc_y;
        acc_x += (i64)s.RelativeMovementX * 4;
        acc_y += (i64)s.RelativeMovementY * 4;
        int dx = (int)(acc_x / rx), dy = (int)(acc_y / ry);
        acc_x -= dx * rx;
        acc_y -= dy * ry;
        cur_x = CLAMP(cur_x + dx, 0, (int)k.fb_w - 1);
        cur_y = CLAMP(cur_y + dy, 0, (int)k.fb_h - 1);
        cur_visible = 1;
        event_t e = { .x = cur_x, .y = cur_y, .from_mouse = 1 };
        if (s.LeftButton && !rel_down) e.type = EV_DOWN;
        else if (!s.LeftButton && rel_down) e.type = EV_UP;
        else if (dx || dy) e.type = EV_MOVE;
        rel_down = s.LeftButton;
        if (e.type) out[n++] = e;
        if (s.RelativeMovementZ) {
            event_t w = { .type = EV_SCROLL, .x = cur_x, .y = cur_y,
                          .dy = s.RelativeMovementZ > 0 ? -1 : 1, .from_mouse = 1 };
            out[n++] = w;
        }
    }

    EFI_INPUT_KEY key;
    while (n < max && !EFI_ERROR(k.st->ConIn->ReadKeyStroke(k.st->ConIn, &key))) {
        if (inject_key(key.UnicodeChar, &out[n])) {
            if (out[n].type) n++;
            continue;
        }
        event_t e = { .type = EV_KEY, .scan = key.ScanCode, .ch = key.UnicodeChar };
        out[n++] = e;
    }
    return n;
}

/* ---- storage ---------------------------------------------------------- */
static void probe_storage(void) {
    UINTN n = 0;
    EFI_HANDLE *h = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    k.bs->HandleProtocol(k.image, &li_guid, (void **)&li);

    if (!EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &blk_guid, NULL, &n, &h))) {
        for (UINTN i = 0; i < n && k.n_blk < MAX_BLK; i++) {
            EFI_BLOCK_IO_PROTOCOL *b;
            if (EFI_ERROR(k.bs->HandleProtocol(h[i], &blk_guid, (void **)&b))) continue;
            if (!b->Media->MediaPresent) continue;
            blockdev_t *d = &k.blk[k.n_blk++];
            d->bytes = (b->Media->LastBlock + 1) * b->Media->BlockSize;
            d->removable = b->Media->RemovableMedia;
            d->partition = b->Media->LogicalPartition;
            d->read_only = b->Media->ReadOnly;
        }
        k.bs->FreePool(h);
    }

    if (!EFI_ERROR(k.bs->LocateHandleBuffer(ByProtocol, &sfs_guid, NULL, &n, &h))) {
        for (UINTN i = 0; i < n && k.n_vol < MAX_VOL; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
            EFI_FILE_PROTOCOL *root;
            if (EFI_ERROR(k.bs->HandleProtocol(h[i], &sfs_guid, (void **)&fs))) continue;
            if (EFI_ERROR(fs->OpenVolume(fs, &root))) continue;
            volume_t *v = &k.vol[k.n_vol++];
            v->fs = fs;
            v->boot = li && li->DeviceHandle == h[i];
            u8 buf[512];
            UINTN sz = sizeof buf;
            if (!EFI_ERROR(root->GetInfo(root, &fsinfo_guid, &sz, buf))) {
                EFI_FILE_SYSTEM_INFO *fi = (EFI_FILE_SYSTEM_INFO *)buf;
                v->size = fi->VolumeSize;
                v->free = fi->FreeSpace;
                v->read_only = fi->ReadOnly;
                str16_to_utf8(v->label, sizeof v->label, fi->VolumeLabel);
            }
            if (!v->label[0]) fmt(v->label, sizeof v->label, "Volume %d", k.n_vol);
            root->Close(root);
            char sz_txt[24];
            fmt_bytes(sz_txt, sizeof sz_txt, v->size);
            klog("storage: volume '%s' %s%s", v->label, sz_txt, v->boot ? " (boot)" : "");
        }
        k.bs->FreePool(h);
    }
    klog("storage: %d block devices, %d mounted volumes", k.n_blk, k.n_vol);
}

void hal_set_touch_map(u32 map) {
    k.touch_map = map & 7;
    hal_setting_set(u"QrtTouchMap", k.touch_map);
}

void hal_probe(void) {
    k.touch_map = hal_setting_get(u"QrtTouchMap", 0) & 7;
    probe_display();
    probe_input();
    probe_storage();
}

/* ---- power ------------------------------------------------------------- */
void hal_shutdown(void) { k.rt->ResetSystem(EfiResetShutdown, 0, 0, NULL); }
void hal_reboot(void)   { k.rt->ResetSystem(EfiResetCold, 0, 0, NULL); }

int hal_reboot_to_firmware(void) {
    u64 supported = 0, ind = 0;
    UINTN sz = sizeof supported;
    if (EFI_ERROR(k.rt->GetVariable(u"OsIndicationsSupported", &global_guid, NULL, &sz, &supported)) ||
        !(supported & 1))
        return 0;
    sz = sizeof ind;
    k.rt->GetVariable(u"OsIndications", &global_guid, NULL, &sz, &ind);
    ind |= 1;   /* EFI_OS_INDICATIONS_BOOT_TO_FW_UI */
    if (EFI_ERROR(k.rt->SetVariable(u"OsIndications", &global_guid,
                                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                    EFI_VARIABLE_RUNTIME_ACCESS, sizeof ind, &ind)))
        return 0;
    hal_reboot();
    return 1;
}

/* ---- settings in NVRAM ------------------------------------------------ */
u32 hal_setting_get(const c16 *name, u32 def) {
    u32 v = def;
    UINTN sz = sizeof v;
    if (EFI_ERROR(k.rt->GetVariable(name, &qrt_guid, NULL, &sz, &v)) || sz != sizeof v) return def;
    return v;
}

void hal_setting_set(const c16 *name, u32 value) {
    k.rt->SetVariable(name, &qrt_guid,
                      EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                      sizeof value, &value);
}
