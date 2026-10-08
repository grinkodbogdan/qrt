/*
 * main.c - Tessera on 64-bit ARM: from the boot loader to the shell.
 *
 * arm_main gets the device tree from boot.S and sets up what the shared kernel code
 * expects of a native kernel (k.native = 1): memory from the tree's /memory nodes minus
 * the firmware's reserved regions, the MMU and caches, the heap, a clock (the ARM
 * generic timer), the display, input, the file tree (the boot image's ramdisk, a cpio
 * archive), then the shell.  One core, no interrupts: the shell's loop polls, as
 * Tessera did on the firmware before x86-64's native kernel.
 */
#include "arm.h"
#include "../../kernel/vfs.h"
#include "../../ui/shell.h"
#include "sched.h"

kernel_t k;
u64 arm_ram_base, arm_ram_size;
u64 arm_dma_pool_base, arm_dma_pool_size;
const char *arm_model = "ARM64 board";

/* ---- clock: the generic timer's virtual counter ---- */
static u64 cnt_freq = 1, cnt_boot;
static u64 cntvct(void) { u64 v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
u64 k_now_ms(void) { return (cntvct() - cnt_boot) * 1000 / cnt_freq; }
u64 k_now_us(void) { u64 d = cntvct() - cnt_boot; return d / cnt_freq * 1000000 + d % cnt_freq * 1000000 / cnt_freq; }

/* ---- interrupts are never on yet; the shared code still brackets with these ---- */
u64 irq_save(void) { u64 f; __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(f) :: "memory"); return f; }
void irq_restore(u64 f) { __asm__ volatile("msr daif, %0" : : "r"(f) : "memory"); }

/* ---- the console: QEMU's PL011 when the tree has one ---- */
static u64 pl011;
int  uart_init(void) { return pl011 != 0; }
int  uart_present(void) { return pl011 != 0; }
void uart_putc(char c) {
    if (!pl011) return;
    while (R32(pl011 + 0x18) & (1 << 5)) {}                    /* TX FIFO full */
    W32(pl011, (u8)c);
}
void uart_write(const char *s) { for (; *s; s++) { if (*s == '\n') uart_putc('\r'); uart_putc(*s); } }
int  uart_getc(void) { if (!pl011 || (R32(pl011 + 0x18) & (1 << 4))) return -1; return (int)(R32(pl011) & 0xff); }
u64  uart_rx_count(void) { return 0; }

extern char _start[], __bss_end[];   /* the image: _start .. __bss_end (both relocated) */

/* ---- crashes: the registers on the console and the screen ---- */
static const char *const kinds[] = { "EL1t sync", "EL1t IRQ", "EL1t FIQ", "EL1t SError", "synchronous exception", "IRQ", "FIQ",
                                     "SError (asynchronous abort)", "EL0 sync", "EL0 IRQ", "EL0 FIQ", "EL0 SError" };
void native_panic(const char *what, void *frame) {
    __asm__ volatile("msr daifset, #0xf");
    klog("*** QRT kernel panic: %s", what);
    if (frame) {
        u64 *r = frame;
        klog("pc %llx  lr %llx  sp %llx  esr %llx  far %llx", r[31], r[30], (u64)(usize)frame + 34 * 8,
             SYSREG_R(esr_el1), SYSREG_R(far_el1));
        for (int i = 0; i < 30; i += 3) klog("x%-2d %016llx  x%-2d %016llx  x%-2d %016llx", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2]);
    }
    if (k.fb_base) {                                              /* a red band across the top */
        static u32 red[64 * 64];
        for (int i = 0; i < 64 * 64; i++) red[i] = 0xc01c28;
        for (u32 x = 0; x < k.fb_w; x += 64) fb_present(red, 64, (int)x, 0, 64, 64);
    }
    for (;;) __asm__ volatile("wfe");
}
void linux_failed(const char *why);
void arm_exception(u64 kind, u64 *frame) {
    char m[96];
    fmt(m, sizeof m, "%s at %llx (ESR %llx, address %llx)", kind < 12 ? kinds[kind] : "exception", frame[31],
        SYSREG_R(esr_el1), SYSREG_R(far_el1));
    if (!thr_is_main()) {                                         /* a Linux thread: Linux stops, the shell goes on */
        klog("*** fault in a driver thread: %s", m);
        klog("pc %llx  lr %llx  (kernel loaded at %llx)", frame[31], frame[30], (u64)(usize)_start);
        linux_failed(m);
        thr_park();
    }
    native_panic(m, frame);
}

/* ---- memory map from the device tree ---- */
#define MAXR 32
static u64 ram[MAXR][2], hole[MAXR * 2][2];
static int nram, nhole;

int arm_is_ram(u64 a, u64 len) {
    for (int i = 0; i < nram; i++) if (a >= ram[i][0] && a + len <= ram[i][0] + ram[i][1]) return 1;
    return 0;
}
static void add_hole(u64 a, u64 s) { if (s && nhole < MAXR * 2) { hole[nhole][0] = a; hole[nhole][1] = s; nhole++; } }

static int mem_one(int c, void *arg) {
    (void)arg;
    if (strncmp(fdt_name(c), "memory", 6)) return 0;
    u64 a, s;
    for (int i = 0; i < MAXR && nram < MAXR && fdt_reg(c, i, &a, &s); i++)
        if (s) { ram[nram][0] = a; ram[nram][1] = s; nram++; }
    return 0;
}
/* reserved regions Linux's drivers read like memory (shared memory with the RPM and the
 * modem, the remote file system buffer) are mapped uncached-normal, not device memory */
static u64 shared[8][2];
static int nshared;
static u64 ramoops[2];
static int rsv_one(int c, void *arg) {
    (void)arg;
    u64 a, s;
    int len;
    const char *compat = fdt_prop(c, "compatible", &len);
    int oops = compat && !strcmp(compat, "ramoops");
    int mem = compat && (!strcmp(compat, "qcom,smem") || !strcmp(compat, "qcom,rmtfs-mem") || oops);
    if (oops && fdt_reg(c, 0, &a, &s)) { ramoops[0] = a; ramoops[1] = s; }
    for (int i = 0; i < 4 && fdt_reg(c, i, &a, &s); i++) {
        add_hole(a, s);
        if (mem && nshared < 8) { shared[nshared][0] = a; shared[nshared][1] = s; nshared++; }
    }
    return 0;
}

/* RAM: the /memory nodes; holes: /reserved-memory, the /memreserve/ block, the tree */
static void scan_memory(const void *dtb) {
    fdt_children(0, mem_one, NULL);
    int rm = fdt_node("/reserved-memory");
    if (rm >= 0) fdt_children(rm, rsv_one, NULL);
    const u8 *b = dtb;
    for (const u8 *e = b + fdt_u32(b + 16); fdt_u64(e + 8); e += 16) add_hole(fdt_u64(e), fdt_u64(e + 8));
    add_hole((u64)(usize)dtb, fdt_size());
}

static u64 initrd_start, initrd_end;
static void scan_chosen(void) {
    int ch = fdt_node("/chosen"), len;
    if (ch < 0) return;
    const void *p = fdt_prop(ch, "linux,initrd-start", &len);
    if (p) initrd_start = len == 8 ? fdt_u64(p) : fdt_u32(p);
    p = fdt_prop(ch, "linux,initrd-end", &len);
    if (p) initrd_end = len == 8 ? fdt_u64(p) : fdt_u32(p);
    if (initrd_end > initrd_start) add_hole(initrd_start, initrd_end - initrd_start);
}

/* the largest stretch of RAM (2 MB aligned) clear of every hole and of the kernel */
static void free_span(u64 img_lo, u64 img_hi, u64 *lo, u64 *hi) {
    *lo = *hi = 0;
    add_hole(img_lo, img_hi - img_lo);
    for (int r = 0; r < nram; r++) {
        u64 a = ram[r][0], end = ram[r][0] + ram[r][1];
        while (a < end) {
            u64 b = end, skip = end;
            for (int h = 0; h < nhole; h++) {                       /* the first hole at or after a */
                u64 ha = hole[h][0], he = hole[h][0] + hole[h][1];
                if (he <= a || ha >= b) continue;
                b = ha > a ? ha : a;
                skip = he;
            }
            if (b > (4ull << 30)) b = 4ull << 30;                   /* below 4 GB: 32-bit DMA */
            u64 al = (a + 0x1fffff) & ~0x1fffffull, bl = b & ~0x1fffffull;
            if (bl > al && bl - al > *hi - *lo) { *lo = al; *hi = bl; }
            a = skip > a ? skip : end;
        }
    }
}

/* ---- the ramdisk: a "newc" cpio archive -> the file tree ---- */
static u32 hex8(const char *s) { u32 v = 0; for (int i = 0; i < 8; i++) { char c = s[i]; v = v << 4 | (u32)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10); } return v; }
static int load_cpio(const u8 *p, const u8 *end) {
    int files = 0;
    while (p + 110 <= end && !memcmp(p, "070701", 6)) {
        const char *h = (const char *)p;
        u32 mode = hex8(h + 14), fsize = hex8(h + 54), nsize = hex8(h + 94);
        const char *name = h + 110;
        if (!strcmp(name, "TRAILER!!!")) break;
        const u8 *data = p + ((110 + nsize + 3) & ~3u);
        char path[256] = "/";
        strlcat(path, name[0] == '.' && name[1] == '/' ? name + 2 : name, sizeof path);
        if ((mode & 0170000) == 0040000) vfs_create(path, 1);
        else if ((mode & 0170000) == 0100000) {
            vnode_t *n = vfs_create(path, 0);
            if (n) { vfs_write(n, 0, data, fsize); files++; }
        }
        p = data + ((fsize + 3) & ~3u);
    }
    return files;
}

void hal_arm_init(void);

void arm_main(const void *dtb, u64 base) {
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cnt_freq));
    if (!cnt_freq) cnt_freq = 19200000;
    cnt_boot = cntvct();
    k.native = 1;
    if (!fdt_init(dtb)) for (;;) __asm__ volatile("wfe");          /* nothing to go on */
    int n = fdt_find_compatible(-1, "arm,pl011");
    u64 a, s;
    if (n >= 0 && fdt_reg(n, 0, &a, &s)) pl011 = a;
    int len;
    const char *model = fdt_prop(0, "model", &len);
    if (model) arm_model = model;
    klog("QRT %s (arm64) - Tessera kernel on %s, loaded at %llx", QRT_VERSION, arm_model, base);
    scan_memory(dtb);
    scan_chosen();
    for (int i = 0; i < nram; i++) { klog("memory: %llx-%llx", ram[i][0], ram[i][0] + ram[i][1]); k.ram_bytes += ram[i][1]; }
    for (int i = 0; i < nhole; i++) klog("reserved: %llx-%llx", hole[i][0], hole[i][0] + hole[i][1]);
    if (!nram) panic("no /memory in the device tree");

    u64 img_lo = (u64)(usize)_start, img_hi = img_lo + (u64)(__bss_end - _start);
    if (!fb_init()) klog("display: none found");                    /* before the MMU: device reads */
    extern u64 fb_reserve_base, fb_reserve_size;
    add_hole(fb_reserve_base, fb_reserve_size);
    u64 lo, hi;
    free_span(img_lo, img_hi, &lo, &hi);
    if (hi - lo < (64ull << 20)) panic("less than 64 MB of free RAM");
    /* the top 16 MB of it: uncached, Linux's coherent DMA buffers */
    arm_dma_pool_size = 16ull << 20;
    hi -= arm_dma_pool_size;
    arm_dma_pool_base = hi;
    u64 nc[10][2] = { { fb_reserve_base, fb_reserve_size }, { arm_dma_pool_base, arm_dma_pool_size } };
    int nnc = 2;
    for (int i = 0; i < nshared; i++) { nc[nnc][0] = shared[i][0]; nc[nnc][1] = shared[i][1]; nnc++; }
    mmu_init((const u64 (*)[2])ram, nram, (const u64 (*)[2])nc, nnc, (const u64 (*)[2])hole, nhole);
    pmm_init(lo, hi);
    if (ramoops[1]) {                                               /* the log across resets */
        plog_init(ramoops[0], ramoops[1]);
        for (int i = 0; klog_line(i); i++) plog_line(klog_line(i));
    }
    arm_ram_base = lo; arm_ram_size = hi - lo;
    klog("memory: %llu MB in all, heap and pages %llx-%llx (%llu MB)", k.ram_bytes >> 20, lo, hi, (hi - lo) >> 20);
    klog("display: %s, %u x %u", fb_what, k.fb_w, k.fb_h);
    k.graphics_up = 0;
    if (initrd_end > initrd_start) {
        int files = load_cpio((const u8 *)(usize)initrd_start, (const u8 *)(usize)initrd_end);
        klog("ramdisk: %d files (%llu KB)", files, (initrd_end - initrd_start) >> 10);
    }
    hal_arm_init();
    time_init();
    /* Linux's drivers, on threads - unless the last boot reset the phone while they were
     * starting: then this boot stays without them and shows that boot's log */
    int ch = fdt_node("/chosen"), alen;
    const char *bootargs = ch >= 0 ? fdt_prop(ch, "bootargs", &alen) : NULL;
    if (bootargs && strstr(bootargs, "qrt.mmucheck")) {               /* each reserved region's edges */
        int mmu_attr_at(u64 pa);
        static const char *const t[] = { "device", "cached", "uncached" };
        for (int i = 0; i < nhole; i++) {
            u64 s0 = hole[i][0] & ~0xfffull, e = (hole[i][0] + hole[i][1] + 0xfff) & ~0xfffull;
            klog("mmu: %llx-%llx: before %s, first %s, last %s, after %s", hole[i][0], hole[i][0] + hole[i][1],
                 t[mmu_attr_at(s0 - 4096) % 3], t[mmu_attr_at(s0) % 3], t[mmu_attr_at(e - 4096) % 3], t[mmu_attr_at(e) % 3]);
        }
    }
    if (bootargs && strstr(bootargs, "qrt.nolinux")) klog("linux: off (qrt.nolinux)");
    else if (plog_last_boot_failed()) {
        klog("safe boot: the last boot reset while Linux's drivers were starting (%d lines kept); Linux stays off",
             plog_prev_lines());
        logview_show_previous();
    } else linux_start(dtb);
    shell_main();
    panic("the shell returned");
}
