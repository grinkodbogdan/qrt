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
#include "../../kernel/dev.h"
#include "irq.h"
#include "../../drivers/pci.h"
#include "../../drivers/touch.h"
#include "../../drivers/battery.h"
#include "../../drivers/pmic.h"
#include "../../drivers/audio.h"
#include "../../ui/gfx.h"
#include "sched.h"
#include "proc.h"
void smp_programs_start(void);
int native_smp_workers(void);

extern void switch_stack(u64 top, void (*fn)(void *), void *arg);
void native_smp_start(void);
void sched_init(void);
void vfs_relocate(void);
void proc_init(void);

static mm_boot_t boot;
static u64 trampoline_page;
u64 native_trampoline(void) { return trampoline_page; }

/* ---- crash screen ---------------------------------------------------------- */
static int in_panic;

/* the kernel's function names (tools/symtab.py: a table the second link adds) */
extern const unsigned qrt_nsyms, qrt_sym_off[], qrt_sym_name[];
extern const char qrt_sym_str[];

/* "thread_wake+0x3" for an address in the kernel, else "" */
static void sym_of(u64 a, char *out, usize cap);
void kernel_symbol(u64 a, char *out, usize cap) { sym_of(a, out, cap); if (!out[0]) fmt(out, cap, "%llx", a); }
static void sym_of(u64 a, char *out, usize cap) {
    out[0] = 0;
    if (!k.image_base || a < k.image_base || a >= k.image_base + k.image_size) return;
    u64 off = a - k.image_base;
    int lo = 0, hi = (int)qrt_nsyms - 1, best = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (qrt_sym_off[m] <= off) { best = m; lo = m + 1; } else hi = m - 1; }
    if (best < 0) fmt(out, cap, "k+%llx", off);
    else fmt(out, cap, "%s+%llx", qrt_sym_str + qrt_sym_name[best], off - qrt_sym_off[best]);
}

void native_panic(const char *what, frame_t *f) {
    cli();
    if (in_panic++) for (;;) hlt();
    char line[200], sym[64];
    fmt(line, sizeof line, "\n*** QRT kernel panic: %s\n", what);
    uart_write(line);
    canvas_t c = { (u32 *)(usize)k.fb_base, (int)k.fb_w, (int)k.fb_h, (int)k.fb_stride,
                   { 0, 0, (int)k.fb_w, (int)k.fb_h }, { 0, 0, (int)k.fb_w, (int)k.fb_h } };
    const font_t *big = font_pick(F_SEMIBOLD, 40), *sm = font_pick(F_REGULAR, 26);
    int y = c.h / 10, x = c.w / 14;
#define OUT(col) do { uart_write(line); uart_write("\n"); if (k.fb_base) { gfx_text_fit(&c, sm, x, y, c.w - 2 * x, line, col); y += sm->line + 4; } } while (0)
    if (k.fb_base) {
        gfx_fill(&c, (rect_t){ 0, 0, c.w, c.h }, RGB(0x2a, 0x0a, 0x18));
        gfx_text(&c, big, x, y, "QRT stopped", RGB(255, 255, 255));
        y += big->line * 2;
    }
    strlcpy(line, what, sizeof line);
    OUT(RGB(255, 180, 200));
    thread_t *t = thread_current();
    if (t) {
        proc_t *p = t->proc;
        fmt(line, sizeof line, "thread %s%s%s (pid %d)", t->name ? t->name : "?", p ? ", process " : "", p ? p->name : "", p ? p->pid : 0);
        OUT(RGB(230, 230, 240));
    }
    if (f) {
        /* where, by name: the code that faulted, then return addresses found on its stack */
        sym_of(f->rip, sym, sizeof sym);
        fmt(line, sizeof line, "at %s  (rip %llx, cr2 %llx, err %llx, vector %llu)", sym[0] ? sym : "?", f->rip, read_cr2(), f->err, f->vector);
        OUT(RGB(255, 255, 255));
        fmt(line, sizeof line, "rax %llx rbx %llx rcx %llx rdx %llx rsp %llx", f->rax, f->rbx, f->rcx, f->rdx, f->rsp);
        OUT(RGB(180, 180, 200));
        fmt(line, sizeof line, "rsi %llx rdi %llx rbp %llx r8 %llx r12 %llx", f->rsi, f->rdi, f->rbp, f->r8, f->r12);
        OUT(RGB(180, 180, 200));
        if ((f->cs & 3) == 0 && t && f->rsp > t->kstack && f->rsp < t->kstack_top) {
            int n = 0;
            for (u64 a = f->rsp & ~7ull; a + 8 <= t->kstack_top && n < 8; a += 8) {
                u64 v = *(u64 *)(usize)a;
                sym_of(v, sym, sizeof sym);
                if (!sym[0] || sym[0] == 'k') continue;
                fmt(line, sizeof line, "  called from %s", sym);
                OUT(RGB(210, 210, 230));
                n++;
            }
        }
    }
    /* what happened just before */
    y += 8;
    int first = 0;
    while (klog_line(first)) first++;
    for (int i = first > 6 ? first - 6 : 0; i < first; i++) { strlcpy(line, klog_line(i), sizeof line); OUT(RGB(150, 150, 170)); }
    y += 8;
    strlcpy(line, "Hold the power button to turn the tablet off. A photo of this screen tells what to fix.", sizeof line);
    OUT(RGB(200, 200, 210));
#undef OUT
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
    proc_init();                           /* SYSCALL entry, fault handlers for user processes */
    native_smp_start();
    if (hal_setting_get(u"QrtSmpPrograms", 0) && native_smp_workers()) {   /* 0.9.5: programs on every core */
        sched_smp_enable();
        smp_programs_start();
        klog("smp: programs run on all %d cores", native_smp_workers() + 1);
    }

    if (nt.primary >= 0 && !ntouch_native_resume()) {
        /* We cannot go back to the firmware now.  Remember the failure and
         * reboot; the next boot stays in firmware mode. */
        klog("native: touchscreen did not come back - rebooting into firmware mode");
        hal_setting_set(u"QrtBootMode", 2);
        k_delay_us(500000);
        hal_reboot();
    }
    klog("native: %s", nt.status[0] ? nt.status : "touch not present");
    battery_native_resume();
    pmic_native_resume();
    audio_native_resume();
    strlcpy(k.boot_note, "Native kernel", sizeof k.boot_note);
    irq_init();                            /* I/O APICs from the MADT, every line masked */
    dev_init();                            /* enumerate PCI/ACPI/platform devices, bind drivers */
    shell_main();
}
