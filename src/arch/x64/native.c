/*
 * native.c - leaving the firmware behind.
 *
 *   native_prepare()  still under UEFI: decide whether native mode is safe,
 *                     record the framebuffer, reserve low memory for the AP
 *                     trampoline, make settings reachable at runtime.
 *   native_enter()    ExitBootServices(), then take over: our page tables,
 *                     GDT/IDT/TSS, stack, APIC timer, drivers - and run the
 *                     shell.  Never returns.
 *
 * After the handover only UEFI *runtime* services remain (clock, variables,
 * reset); they are callable by design, identity-mapped, from one core.
 */
#include "arch.h"
#include "../../drivers/uart.h"
#include "../../drivers/pci.h"
#include "../../drivers/touch.h"
#include "../../ui/gfx.h"

extern void switch_stack(u64 top, void (*fn)(void *), void *arg);
void native_smp_start(void);
void sched_init(void);
void vfs_relocate(void);

static mm_boot_t boot;
static u64 trampoline_page;
u64 native_trampoline(void) { return trampoline_page; }

/* ---- crash screen ---------------------------------------------------------- */
static int in_panic;

void native_panic(const char *what, frame_t *f) {
    cli();
    if (in_panic++) for (;;) hlt();
    char line[160];
    fmt(line, sizeof line, "\n*** QRT kernel panic: %s\n", what);
    uart_write(line);
    if (k.fb_base) {
        canvas_t c = { (u32 *)(usize)k.fb_base, (int)k.fb_w, (int)k.fb_h, (int)k.fb_stride,
                       { 0, 0, (int)k.fb_w, (int)k.fb_h }, { 0, 0, (int)k.fb_w, (int)k.fb_h } };
        gfx_fill(&c, (rect_t){ 0, 0, c.w, c.h }, RGB(0x2a, 0x0a, 0x18));
        const font_t *big = font_pick(F_SEMIBOLD, 40), *sm = font_pick(F_REGULAR, 20);
        int y = c.h / 6, x = c.w / 12;
        gfx_text(&c, big, x, y, "QRT stopped", RGB(255, 255, 255));
        y += big->line * 2;
        gfx_text(&c, sm, x, y, what, RGB(255, 180, 200));
        y += sm->line * 2;
        if (f) {
            fmt(line, sizeof line, "rip %llx  cs %llx  rflags %llx  err %llx", f->rip, f->cs, f->rflags, f->err);
            gfx_text(&c, sm, x, y, line, RGB(230, 230, 240)); y += sm->line; uart_write(line); uart_write("\n");
            fmt(line, sizeof line, "rsp %llx  cr2 %llx  vector %llu", f->rsp, read_cr2(), f->vector);
            gfx_text(&c, sm, x, y, line, RGB(230, 230, 240)); y += sm->line; uart_write(line); uart_write("\n");
            fmt(line, sizeof line, "rax %llx rbx %llx rcx %llx rdx %llx", f->rax, f->rbx, f->rcx, f->rdx);
            gfx_text(&c, sm, x, y, line, RGB(180, 180, 200)); y += sm->line;
            fmt(line, sizeof line, "rsi %llx rdi %llx rbp %llx r12 %llx", f->rsi, f->rdi, f->rbp, f->r12);
            gfx_text(&c, sm, x, y, line, RGB(180, 180, 200)); y += sm->line * 2;
        }
        gfx_text(&c, sm, x, y, "Hold the power button to turn the tablet off.", RGB(200, 200, 210));
    }
    for (;;) hlt();
}

/* ---- present: copy logical pixels to the panel ------------------------------ */
void native_present(const u32 *src, int stride, int x, int y, int w, int h) {
    for (int r = 0; r < h; r++) {
        u32 *d = (u32 *)(usize)k.fb_base + (usize)(y + r) * k.fb_stride + x;
        const u32 *s = src + (usize)(y + r) * stride + x;
        if (!k.fb_rgb) memcpy(d, s, (usize)w * 4);
        else for (int i = 0; i < w; i++) {
            u32 p = s[i];
            d[i] = (p & 0xff00ff00u) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);
        }
    }
}

/* ---- phase 1: under UEFI ------------------------------------------------------ */
int native_prepare(void) {
    uart_init();
    EFI_GRAPHICS_OUTPUT_MODE *m = k.gop->Mode;
    if (m->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor &&
        m->Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor) {
        klog("native: framebuffer format %d not directly writable", m->Info->PixelFormat);
        return 0;
    }
    if ((u64)(usize)k.image < HIGH_POOL) { /* see mm.c: kernel memory must sit above the user window */ }
    k.fb_base = m->FrameBufferBase;
    k.fb_stride = m->Info->PixelsPerScanLine;
    k.fb_rgb = m->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor;
    boot.fb_base = m->FrameBufferBase;
    boot.fb_size = m->FrameBufferSize;

    EFI_GUID li_guid = LOADED_IMAGE_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    if (!EFI_ERROR(k.bs->HandleProtocol(k.image, &li_guid, (void **)&li))) {
        k.image_base = (u64)(usize)li->ImageBase;
        k.image_size = li->ImageSize;
        klog("native: kernel image at %llx (%llu KB)", k.image_base, k.image_size >> 10);
    }
    u64 page = 0x9f000;                                     /* AP trampoline: below 1 MiB */
    if (EFI_ERROR(k.bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &page))) page = 0;
    trampoline_page = page;
    return 1;
}

/* ---- phase 2: the handover ------------------------------------------------------ */
static void native_main(void *arg);

void native_enter(void) {
    /* Everything below must not call boot services except the two map calls. */
    UINTN size = 0, key = 0, dsz = 0;
    u32 dver = 0;
    k.bs->GetMemoryMap(&size, NULL, &key, &dsz, &dver);
    size += 64 * dsz;
    EFI_MEMORY_DESCRIPTOR *map = kalloc(size);
    k.graphics_up = 1;                                       /* klog: no more ConOut */
    for (int attempt = 0; attempt < 4; attempt++) {
        UINTN s = size;
        if (EFI_ERROR(k.bs->GetMemoryMap(&s, map, &key, &dsz, &dver))) continue;
        if (!EFI_ERROR(k.bs->ExitBootServices(k.image, key))) {
            boot.map = map; boot.map_size = s; boot.desc_size = dsz;
            goto out;
        }
    }
    panic("ExitBootServices failed");
out:
    cli();
    k.native = 1;
    k.bs = NULL;
    k.st->ConIn = NULL;
    k.st->ConOut = NULL;
    k.st->BootServices = NULL;

    /* The firmware's stack, GDT, IDT and page tables live in boot-services
     * memory: keep those regions out of the allocator until we have our own. */
    struct { u16 lim; u64 base; } __attribute__((packed)) gdtr, idtr;
    __asm__ volatile("sgdt %0; sidt %1" : "=m"(gdtr), "=m"(idtr));
    u64 rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    u64 reserve[] = { rsp, gdtr.base, idtr.base, read_cr3() };
    mm_init(&boot, reserve, ARRAY_LEN(reserve));

    u64 stack = pmm_alloc_contig(16);                        /* 64 KiB kernel stack */
    switch_stack(stack + 16 * PAGE, native_main, NULL);
}

static void native_main(void *arg) {
    (void)arg;
    percpu_t *c = heap_alloc(sizeof *c);
    idt_init();
    cpu_setup(c, 0);
    ncpus = 1;
    lapic_init();
    c->apic_id = lapic_id();
    klog("native: firmware exited; %llu MB managed, identity map %llu GB",
         pmm_total_bytes() >> 20, mm_max_phys() >> 30);

    sched_init();                          /* this context becomes the shell thread */
    lapic_timer_start(1000);
    sti();
    vfs_relocate();                        /* file data out of firmware pool memory */
    native_smp_start();

    if (nt.primary >= 0 && !ntouch_native_resume()) {
        /* We cannot go back to the firmware now.  Remember the failure and
         * reboot; the next boot stays in firmware mode. */
        klog("native: touchscreen did not come back - rebooting into firmware mode");
        hal_setting_set(u"QrtBootMode", 2);
        k_delay_us(500000);
        hal_reboot();
    }
    klog("native: %s", nt.status[0] ? nt.status : "touch not present");
    for (int i = 0; i < pci_ndevs; i++)
        if (pci_devs[i].class_code == 0x03) pci_devs[i].driver = "framebuffer (QRT, write-combining)";
    shell_main();
}
