/*
 * hal.c - device discovery and the firmware-backed driver layer.
 */
#include "kernel.h"
#include "../drivers/buttons.h"
#include "../drivers/usb/xhci.h"
#include "../drivers/touch.h"
#include "../drivers/uart.h"
#include "../drivers/i915/display.h"
#if defined(__x86_64__)
#include "../arch/x64/sched.h"
#include "../arch/x64/mm.h"
void native_present(const u32 *src, int stride, int x, int y, int w, int h);
#endif

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
 *   Ctrl-T  <d|m|u>  <x> , <y>  ;      (D, M, U: two fingers)
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
    if (buf[0] == 'v') { display_virtual(x, y); return 1; }   /* tests: plug a monitor of x by y (0: unplug) */
#if defined(__x86_64__)
    if (buf[0] == 'p') { if (k.native) sched_dump_threads(); return 1; }   /* tests: what every program thread is doing */
#endif
    out->x = CLAMP(x, 0, (int)k.fb_w - 1);
    out->y = CLAMP(y, 0, (int)k.fb_h - 1);
    /* upper case: the same with two fingers on the glass (D, M, U) */
    char c = buf[0] >= 'A' && buf[0] <= 'Z' ? (char)(buf[0] + 32) : buf[0];
    out->type = c == 'd' ? EV_DOWN : c == 'u' ? EV_UP : EV_MOVE;
    out->fingers = buf[0] >= 'A' && buf[0] <= 'Z' ? 2 : 1;
    if (out->type == EV_UP) out->fingers = 0;
    return 1;
}

/*
 * Native mode keyboard: the serial port.  Bytes become key events; the
 * Ctrl-T touch packets used by the test harness work exactly as before;
 * ANSI arrow sequences (ESC [ A..D) map to the UEFI scan codes the UI uses.
 */
static int serial_keys(event_t *out, int max) {
    int n = 0, c;
    static int esc, num;
    while (n < max && (c = uart_getc()) >= 0) {
        if (inject_key((c16)c, &out[n])) { if (out[n].type) n++; continue; }
        if (esc == 1) { esc = c == '[' ? 2 : 0; if (!esc) goto plain; continue; }
        if (esc == 2 && c >= '0' && c <= '9') { esc = 3; num = c - '0'; continue; }
        if (esc == 3) {
            /* ESC [ n ~ : F9/F10/F11 stand in for the tablet's volume up/down and power buttons,
             * F12 for power held down, F8 for the Windows button */
            if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); continue; }
            esc = 0;
            u16 sc = c != '~' ? 0 : num == 20 ? SCAN_VOLUP : num == 21 ? SCAN_VOLDN : num == 23 ? SCAN_POWER
                   : num == 24 ? SCAN_POWER_LONG : num == 19 ? SCAN_HOMEBTN : num == 5 ? SCAN_PGUP : num == 6 ? SCAN_PGDN : 0;
            if (sc) out[n++] = (event_t){ .type = EV_KEY, .scan = sc };
            continue;
        }
        if (esc == 2) {
            esc = 0;
            u16 sc = c == 'A' ? SCAN_UP : c == 'B' ? SCAN_DOWN : c == 'C' ? SCAN_RIGHT : c == 'D' ? SCAN_LEFT : 0;
            if (sc) out[n++] = (event_t){ .type = EV_KEY, .scan = sc };
            continue;
        }
        if (c == 0x1b) { esc = 1; continue; }
    plain:
        if (c == 0x7f) c = 8;
        if (c == '\n') c = '\r';
        out[n++] = (event_t){ .type = EV_KEY, .ch = (c16)c };
    }
    if (esc == 1 && n < max) { esc = 0; out[n++] = (event_t){ .type = EV_KEY, .scan = SCAN_ESC }; }
    return n;
}

void hal_reprobe_input(void) {
    k.n_abs = k.n_rel = k.splitter_abs = k.splitter_rel = 0;
    memset(abs_down, 0, sizeof abs_down);
    probe_input();
}

int hal_poll(event_t *out, int max) {
    int n = 0;
    if (cur_x < 0) { cur_x = (int)k.fb_w / 2; cur_y = (int)k.fb_h / 2; }

    int n_abs = k.n_abs ? k.n_abs : k.splitter_abs, n_rel = k.n_rel ? k.n_rel : k.splitter_rel;
    if (k.native) n_abs = n_rel = 0;      /* firmware input drivers are gone */
    if (ntouch_active()) {
        n_abs = 0;                        /* the firmware driver is gone; ours reports */
        n += ntouch_poll(out + n, max - n - 2);
    }
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

    if (k.native) {
        n += buttons_poll(out + n, max - n);
        n += xhci_poll(out + n, max - n);
        return n + serial_keys(out + n, max - n);
    }
    n += buttons_poll(out + n, max - n);                 /* firmware mode reads the pads too */
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
/*
 * Settings are UEFI variables.  Early QRT versions created them as
 * boot-services-only, which makes them invisible once the firmware exits;
 * hal_settings_prepare() re-creates them with runtime access.
 */
#define SETTING_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
static const c16 *setting_names[] = { u"QrtRotation", u"QrtAccent", u"QrtTouchMap", u"QrtSmp", u"QrtBootMode" };

/* the firmware's variable services are not re-entrant, and kernel threads (the display
 * driver's crash guard) write settings too: no preemption during a call */
#if defined(__x86_64__)
#define RT_LOCK   u64 rt_fl = k.native ? irq_save() : 0
#define RT_UNLOCK if (k.native) irq_restore(rt_fl)
#else
#define RT_LOCK   (void)0
#define RT_UNLOCK (void)0
#endif

u32 hal_setting_get(const c16 *name, u32 def) {
    u32 v = def, attr = 0;
    UINTN sz = sizeof v;
    RT_LOCK;
    EFI_STATUS st = k.rt->GetVariable(name, &qrt_guid, &attr, &sz, &v);
    RT_UNLOCK;
    if (EFI_ERROR(st) || sz != sizeof v) return def;
    return v;
}

void hal_setting_set(const c16 *name, u32 value) {
    RT_LOCK;
    k.rt->SetVariable(name, &qrt_guid, SETTING_ATTR, sizeof value, &value);
    RT_UNLOCK;
}

/* byte strings (Wi-Fi network name and key): 0 bytes returned when unset */
usize hal_setting_get_blob(const c16 *name, void *buf, usize cap) {
    u32 attr = 0;
    UINTN sz = cap;
    RT_LOCK;
    EFI_STATUS st = k.rt->GetVariable(name, &qrt_guid, &attr, &sz, buf);
    RT_UNLOCK;
    if (EFI_ERROR(st)) return 0;
    return (usize)sz;
}

void hal_setting_set_blob(const c16 *name, const void *buf, usize len) {
    RT_LOCK;
    k.rt->SetVariable(name, &qrt_guid, len ? SETTING_ATTR : 0, len, (void *)buf);   /* len 0 deletes */
    RT_UNLOCK;
}

void hal_settings_prepare(void) {
    for (usize i = 0; i < ARRAY_LEN(setting_names); i++) {
        u32 v, attr = 0;
        UINTN sz = sizeof v;
        if (EFI_ERROR(k.rt->GetVariable(setting_names[i], &qrt_guid, &attr, &sz, &v)) || sz != sizeof v) continue;
        if (attr & EFI_VARIABLE_RUNTIME_ACCESS) continue;
        k.rt->SetVariable(setting_names[i], &qrt_guid, attr, 0, NULL);          /* delete the old one */
        k.rt->SetVariable(setting_names[i], &qrt_guid, SETTING_ATTR, sizeof v, &v);
    }
}

/* ---- display, timing ---------------------------------------------------- */
void hal_present(const u32 *px, int stride, int x, int y, int w, int h) {
#if defined(__x86_64__)
    if (k.native) { native_present(px, stride, x, y, w, h); return; }
#endif
    k.gop->Blt(k.gop, (u32 *)px, EfiBltBufferToVideo, x, y, x, y, w, h, (UINTN)stride * 4);
}

void hal_wait_frame_ms(u32 ms) {
#if defined(__x86_64__)
    if (k.native) { thread_sleep_ms(ms ? ms : 1); return; }
#endif
    (void)ms;
    hal_wait_frame();                                  /* the firmware's 10 ms timer */
}

void hal_wait_frame(void) {
#if defined(__x86_64__)
    if (k.native) { thread_sleep_ms(10); return; }     /* the CPU halts until then */
#endif
    static EFI_EVENT tick;
    if (!tick) {
        k.bs->CreateEvent(EVT_TIMER, 0, NULL, NULL, &tick);
        k.bs->SetTimer(tick, TimerPeriodic, 100000);   /* 10 ms */
    }
    UINTN idx;
    k.bs->WaitForEvent(1, &tick, &idx);
}

void hal_delay_us(u32 us) {
    if (!k.native) { k.bs->Stall(us); return; }
    u64 end = k_now_us() + us;
    while (k_now_us() < end) __asm__ volatile("pause");
}

/* Device DMA memory: page-aligned, physically contiguous, zeroed, and
 * identity-mapped in both modes (the pointer is the bus address).  In
 * firmware mode it stays below 4 GB for devices with 32-bit fields. */
void *hal_dma_alloc(usize bytes) {
    usize pages = (bytes + 4095) / 4096;
#if defined(__x86_64__)
    if (k.native) return (void *)(usize)pmm_alloc_contig(pages);
#endif
    u64 addr = 0xffffffffull;
    if (EFI_ERROR(k.bs->AllocatePages(AllocateMaxAddress, EfiBootServicesData, pages, &addr))) return NULL;
    memset((void *)(usize)addr, 0, pages * 4096);
    return (void *)(usize)addr;
}

const char *hal_mode(void) { return k.native ? "native kernel" : "firmware-hosted"; }
