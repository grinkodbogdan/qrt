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
/* nanoseconds, to the counter's resolution (52 ns at 19.2 MHz): Linux's udelay() waits on it */
u64 k_now_ns(void) { u64 d = cntvct() - cnt_boot; return d / cnt_freq * 1000000000ull + d % cnt_freq * 1000000000ull / cnt_freq; }

/* ---- interrupts are never on yet; the shared code still brackets with these ---- */
u64 irq_save(void) { u64 f; __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(f) :: "memory"); return f; }
void irq_restore(u64 f) { __asm__ volatile("msr daif, %0" : : "r"(f) : "memory"); }

/* keep a device tree node from Linux: "qcom,..." compatibles become "qrt-,..." in place
 * (same length), so no Linux driver matches it - whatever its status property says */
static void hide_from_linux(int node) {
    int len;
    char *c = node >= 0 ? (char *)fdt_prop(node, "compatible", &len) : NULL;
    for (int i = 0; c && i + 5 <= len; i++)
        if ((i == 0 || c[i - 1] == 0) && !strncmp(c + i, "qcom,", 5)) memcpy(c + i, "qrt-,", 5);
}

/* ---- the console: QEMU's PL011, or a Qualcomm phone's UARTDM (the Mi A1's debug UART
 * at 78af000, on test pads), when the tree has one - every line of the log, Linux's too,
 * goes out from the first one.  UARTDM as Linux's msm_serial writes it (its register
 * offsets: SR 08, CR 10, ISR 14, NCF_TX 40, TF 70): wait for the
 * transmitter, reset TX_READY, say how many characters come, then up to 4 per word.
 * Every wait is bounded: with its clock off the UART must not hang the kernel. ---- */
static u64 pl011, uartdm;
int  uart_init(void) { return pl011 != 0 || uartdm != 0; }
int  uart_present(void) { return pl011 != 0 || uartdm != 0; }
static void dm_send(const char *b, u32 n) {
    for (int t = 0; t < 200000 && !(R32(uartdm + 0x08) & 8) && !(R32(uartdm + 0x14) & 0x80); t++) {}  /* SR TX_EMPTY / ISR TX_READY */
    W32(uartdm + 0x10, 3u << 8);                                       /* CR: reset TX_READY */
    W32(uartdm + 0x40, n);                                            /* NCF_TX */
    (void)R32(uartdm + 0x40);
    for (u32 i = 0; i < n; i += 4) {
        u32 w = 0;
        for (u32 j = 0; j < 4 && i + j < n; j++) w |= (u32)(u8)b[i + j] << (8 * j);
        for (int t = 0; t < 200000 && !(R32(uartdm + 0x08) & 4); t++) {}   /* SR: TX_READY (FIFO room) */
        W32(uartdm + 0x70, w);                                        /* TF */
    }
}
void uart_putc(char c) {
    if (uartdm) { dm_send(&c, 1); return; }
    if (!pl011) return;
    while (R32(pl011 + 0x18) & (1 << 5)) {}                    /* TX FIFO full */
    W32(pl011, (u8)c);
}
/* the Qualcomm UART runs at 115200 baud - ~7 ms a line, which klog spent with interrupts
 * masked: the shell stalled on every line.  Lines go into a ring that a thread drains,
 * yielding while the FIFO is full; before that thread (and in a panic) they go straight out */
#define SRING 65536
static char sring[SRING];
static u32 shead, stail, sdropped;
static volatile int serial_thread;
void uart_flush_now(void) {
    while (stail != shead) {
        char b[64];
        u32 n = 0;
        while (n < sizeof b && stail + n != shead) { b[n] = sring[(stail + n) % SRING]; n++; }
        dm_send(b, n);
        stail += n;
    }
}
/* the UART's clocks running (GCC's CBCR bit 31: CLK_OFF) - Linux's I2C driver, idle,
 * switches off the BLSP bus clock the UART shares, and an access then aborts */
static u64 uart_gcc;
static int uart_clocked(void) { return !uart_gcc || (!(R32(uart_gcc + 0x203c) & (1u << 31)) && !(R32(uart_gcc + 0x1008) & (1u << 31))); }
static void serial_loop(void *a) {
    (void)a;
    for (;;) {
        if (stail == shead || !uart_clocked()) { thr_sleep_us(20000); continue; }
        char b[64];
        u32 n = 0;
        while (n < sizeof b && stail + n != shead) { b[n] = sring[(stail + n) % SRING]; n++; }
        int ok = 1;
        for (int t = 0; t < 1000 && (ok = uart_clocked()) && !(R32(uartdm + 0x08) & 8) && !(R32(uartdm + 0x14) & 0x80); t++) thr_sleep_us(200);
        u64 f = irq_save();                                            /* checked and sent without a switch between */
        if (ok && uart_clocked()) { dm_send(b, n); stail += n; }      /* 64 bytes: the FIFO takes them */
        irq_restore(f);
    }
}
void serial_start(void) { if (uartdm && !serial_thread) { serial_thread = 1; thr_create("serial", serial_loop, NULL, 16 << 10); } }
void uart_write(const char *s) {
    if (uartdm) {                                                     /* into the ring, with \r */
        for (; *s; s++) {
            if (*s == '\n') { if (shead - stail < SRING) sring[shead++ % SRING] = '\r'; else sdropped++; }
            if (shead - stail < SRING) sring[shead++ % SRING] = *s; else sdropped++;
        }
        if (!serial_thread) uart_flush_now();                        /* early boot: no threads yet */
        return;
    }
    for (; *s; s++) { if (*s == '\n') uart_putc('\r'); uart_putc(*s); }
}
int  uart_getc(void) { if (!pl011 || (R32(pl011 + 0x18) & (1 << 4))) return -1; return (int)(R32(pl011) & 0xff); }
u64  uart_rx_count(void) { return 0; }

extern char _start[], __bss_end[];   /* the image: _start .. __bss_end (both relocated) */

/* ---- crashes: the registers on the console and the screen ---- */
static const char *const kinds[] = { "EL1t sync", "EL1t IRQ", "EL1t FIQ", "EL1t SError", "synchronous exception", "IRQ", "FIQ",
                                     "SError (asynchronous abort)", "EL0 sync", "EL0 IRQ", "EL0 FIQ", "EL0 SError" };
void native_panic(const char *what, void *frame) {
    /* IRQs off, FIQs left on: Qualcomm's secure world services its watchdog on FIQs -
     * with them masked too the phone reset seconds after a panic and took the screen
     * with the panic on it */
    __asm__ volatile("msr daifset, #0x2");
    klog("*** QRT kernel panic: %s", what);
    if (frame) {
        u64 *r = frame;
        klog("pc %llx  lr %llx  sp %llx  esr %llx  far %llx", r[31], r[30], (u64)(usize)frame + 34 * 8,
             SYSREG_R(esr_el1), SYSREG_R(far_el1));
        u64 b0 = (u64)(usize)_start, b1 = (u64)(usize)__bss_end;
        klog("in the image: pc +%llx  lr +%llx  (version %s)", r[31] >= b0 && r[31] < b1 ? r[31] - b0 : 0,
             r[30] >= b0 && r[30] < b1 ? r[30] - b0 : 0, QRT_VERSION);
        for (int i = 0; i < 30; i += 3) klog("x%-2d %016llx  x%-2d %016llx  x%-2d %016llx", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2]);
    }
    if (uartdm && uart_clocked()) uart_flush_now();               /* the panic's lines out on the UART */
    if (k.fb_base) { void logview_panic(void); logview_panic(); }   /* the panic's lines on the screen */
    if (k.fb_base) {                                              /* a red band across the top */
        static u32 red[2048 * 32];
        for (int i = 0; i < 2048 * 32; i++) red[i] = 0xc01c28;
        fb_present(red, 2048, 0, 0, (int)MIN(k.fb_w, 2048u), 8);    /* a thin one: the log under it stays readable */
        void fb_flush(void);
        fb_flush();
    }
    for (;;) __asm__ volatile("wfe");
}
void linux_failed(const char *why);
void gic_irq(void);
int  gic_init(void);
void argon_irq_tail(u64 pc);
void arm_exception(u64 kind, u64 *frame) {
    if (kind == 5) {                                              /* EL1h IRQ: the scheduler's tick */
        gic_irq();
        argon_irq_tail(frame[31]);                                /* Linux's own interrupts, as a CPU takes them */
        return;
    }
    char m[96];
    fmt(m, sizeof m, "%s at %llx (ESR %llx, address %llx)", kind < 12 ? kinds[kind] : "exception", frame[31],
        SYSREG_R(esr_el1), SYSREG_R(far_el1));
    if (!thr_is_main()) {                                         /* a thread: it stops, the shell goes on */
        const char *nm = thr_name(thr_self());
        u64 b0 = (u64)(usize)_start;
        klog("*** fault in thread \"%s\": %s", nm ? nm : "?", m);
        klog("pc %llx  lr %llx  (kernel loaded at %llx; in the image pc +%llx lr +%llx)", frame[31], frame[30], b0,
             frame[31] - b0, frame[30] - b0);
        /* only Linux's own threads take Linux down: a fault in one of Tessera's (the serial
         * console in 0.21.15) stopped Linux - and the backlight, the battery - with it */
        if (nm && !strncmp(nm, "linux", 5)) linux_failed(m);
        else if (nm && !strcmp(nm, "serial")) { uartdm = 0; klog("console: the UART is off for this boot"); }
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
    if (!pl011) {                                                 /* the boot loader's UART (enabled in the tree) */
        int u = fdt_find_compatible(-1, "qcom,msm-uartdm-v1.4");
        if (u < 0) u = fdt_find_compatible(-1, "qcom,msm-uartdm");
        int sl;
        const char *st = u >= 0 ? fdt_prop(u, "status", &sl) : NULL;
        /* only with its clocks running (an unclocked BLSP block can hang the bus): known
         * for the MSM8953's BLSP1 UART1 - GCC's CBCR bit 31 is CLK_OFF */
        int g = fdt_find_compatible(-1, "qcom,gcc-msm8953");
        u64 gcc = 0, gs;
        int clocked = g >= 0 && fdt_reg(g, 0, &gcc, &gs) && u >= 0 && fdt_reg(u, 0, &a, &s) && a == 0x78af000 &&
                      !(R32(gcc + 0x203c) & (1u << 31)) && !(R32(gcc + 0x1008) & (1u << 31));
        if (u >= 0 && clocked && (!st || !strcmp(st, "okay"))) {
            uartdm = a;
            uart_gcc = gcc;
            hide_from_linux(u);              /* Tessera's now: Linux's msm_serial would reprogram it under Tessera's writes */
        }
    }
    int len;
    const char *model = fdt_prop(0, "model", &len);
    if (model) arm_model = model;
    klog("QRT %s (arm64) - Tessera kernel on %s, loaded at %llx", QRT_VERSION, arm_model, base);
    if (uartdm) klog("console: the Qualcomm UART at %llx, 115200 8N1 (Linux's msm_serial is off: the UART is Tessera's)", uartdm);
    else if (!pl011 && fdt_find_compatible(-1, "qcom,msm-uartdm") >= 0) klog("console: the Qualcomm UART's clocks are off (the boot loader did not use it): no serial log");
    scan_memory(dtb);
    scan_chosen();
    for (int i = 0; i < nram; i++) { klog("memory: %llx-%llx", ram[i][0], ram[i][0] + ram[i][1]); k.ram_bytes += ram[i][1]; }
    for (int i = 0; i < nhole; i++) klog("reserved: %llx-%llx", hole[i][0], hole[i][0] + hole[i][1]);
    if (!nram) panic("no /memory in the device tree");

    u64 img_lo = (u64)(usize)_start, img_hi = img_lo + (u64)(__bss_end - _start);
    if (!fb_init()) klog("display: none found");                    /* before the MMU: device reads */
    extern u64 fb_reserve_base, fb_reserve_size;
    add_hole(fb_reserve_base, fb_reserve_size);
    extern u64 fb_other[4][2];
    extern int fb_nother;
    for (int i = 0; i < fb_nother; i++) add_hole(fb_other[i][0], fb_other[i][1]);   /* in case a layer stays on */
    /* the log kept across a reset: the last MB of RAM below 4 GB, clear of every reserved
     * region - not the tree's ramoops region, which Qualcomm's firmware fills with its own
     * debug data ("DBGC") after a watchdog reset, so the last boot's log was gone */
    u64 keep = 0;
    for (int r = 0; r < nram; r++) {
        u64 e = ram[r][0] + ram[r][1];
        if (e > (4ull << 30)) e = 4ull << 30;
        if (e < ram[r][0] + (2ull << 20)) continue;
        u64 a = e - (1ull << 20);
        int clear = 1;
        for (int h = 0; h < nhole; h++) if (a < hole[h][0] + hole[h][1] && hole[h][0] < e) clear = 0;
        if (clear && a > keep) keep = a;
    }
    if (keep) { add_hole(keep, 1ull << 20); ramoops[0] = keep; ramoops[1] = 1ull << 20; }
    u64 lo, hi;
    free_span(img_lo, img_hi, &lo, &hi);
    if (hi - lo < (64ull << 20)) panic("less than 64 MB of free RAM");
    /* the top 16 MB of it: uncached, Linux's coherent DMA buffers */
    arm_dma_pool_size = 16ull << 20;
    hi -= arm_dma_pool_size;
    arm_dma_pool_base = hi;
    u64 nc[10][2] = { { arm_dma_pool_base, arm_dma_pool_size } };
    int nnc = 1;
    for (int i = 0; i < nshared; i++) { nc[nnc][0] = shared[i][0]; nc[nnc][1] = shared[i][1]; nnc++; }
    if (fb_reserve_size) { nc[nnc][0] = fb_reserve_base; nc[nnc][1] = fb_reserve_size; nnc++; }   /* the framebuffer: uncached, as in 0.17.0 */
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
    if (gic_init()) __asm__ volatile("msr daifclr, #2");          /* preemption from here on */
    serial_start();                                               /* the UART's lines from a thread now */
    void fb_start_watch(void);
    fb_start_watch();
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
    /* Argon: postmarketOS's tissot device tree names its panel "xiaomi,tissot-panel" (lk2nd
     * fills in the real one); the boot loader reports which it is - "...otm1911_fhd_video" -
     * so Tessera writes the matching driver's compatible in place before Linux reads it */
    int pnode = fdt_find_compatible(-1, "xiaomi,tissot-panel");
    if (pnode >= 0) {
        static const char *const panels[][2] = { { "otm1911_fhd", "mdss,otm1911-fhd" }, { "ili7807_fhd", "mdss,ili7807-fhd" },
                                                 { "ft8716_fhd", "mdss,ft8716-fhd" } };
        int plen;
        char *pc = (char *)fdt_prop(pnode, "compatible", &plen);
        const char *which = NULL;
        for (int i = 0; i < 3 && bootargs && !which; i++) if (strstr(bootargs, panels[i][0])) which = panels[i][1];
        if (pc && which && (int)strlen(which) < plen) {
            memset(pc, 0, (usize)plen);
            memcpy(pc, which, strlen(which));
            klog("argon: panel %s (the boot loader's)", which);
        } else klog("argon: panel not named by the boot loader; Linux will not drive the display");
    }
    /* the display: Tessera draws into the boot loader's (as 0.17.0 and 0.21.4 did - clean).
     * Linux's MSM display driver resets the panel when it probes and, on the Mi A1, has not
     * brought it back yet (0.21.5: the screen faded to black); it is Linux's only with
     * qrt.linuxdisplay on the command line (fastboot boot -c "... qrt.linuxdisplay") */
    int mdss = fdt_find_compatible(-1, "qcom,mdss");
    if (mdss >= 0 && !(bootargs && strstr(bootargs, "qrt.linuxdisplay"))) {
        hide_from_linux(mdss);
        klog("argon: the display stays Tessera's (Linux's display driver off; qrt.linuxdisplay turns it on)");
    }
    /* Linux gets what QRT uses - touch, keys, backlight, battery gauge, eMMC and the clock,
     * power and PMIC plumbing under them - like 0.17-0.19, which never reset.  Kept from it:
     * what goes through Qualcomm's secure world (IOMMU, GPU, video, modem, DSP, audio,
     * camera, IPA - not Wi-Fi: lwifi.c starts it when it is turned on), and what can reset or power off the phone by itself: Linux's
     * PS_HOLD restart/power-off driver (a thermal "critical" shutdown, any reboot inside
     * Linux, pulls it), the thermal sensors, the charger, the display's LAB/IBB rails, USB
     * and haptics.  qrt.allhw gives Linux all of it */
    if (!(bootargs && strstr(bootargs, "qrt.allhw"))) {
        static const char *const kept[] = {
            /* the secure world */
            "qcom,msm-iommu-v1", "qcom,msm-iommu-v2", "qcom,adreno", "qcom,msm8953-venus", "qcom,msm8953-mss-pil",
            "qcom,msm8953-adsp-pil", "qcom,smp2p", "qcom,memshare",
            "qcom,rmtfs-mem", "qcom,apr-v2", "qcom,msm8953-qdsp6-sndcard", "qcom,msm8916-wcd-digital-codec",
            "qcom,pm8916-wcd-analog-codec", "qcom,ipa-lite-v2.6", "qcom,msm8953-camss", "qcom,msm8974-cci",
            /* what can reset or power off the phone */
            "qcom,pshold", "qcom,msm8953-tsens", "qcom,spmi-temp-alarm", "qcom,pmi8996-smbchg", "qcom,pmi8998-lab-ibb",
            "qcom,msm8953-dwc3", "qcom,msm8953-qusb2-phy", "qcom,pmi8950-haptics", "qcom,msm8953-cpr4pd",
            "qcom,apcs-cc-msm8953", NULL };
        int hidden = 0;
        /* Wi-Fi stays Linux's: the Pronto core, its SMD driver, SMSM, SCM and the Pronto core's
         * own smp2p link - started only when Wi-Fi is turned on (lwifi.c) */
        for (int i = 0; kept[i]; i++)
            for (int n = fdt_find_compatible(-1, kept[i]); n >= 0; n = fdt_find_compatible(n, kept[i])) {
                if (!strcmp(fdt_name(n), "smp2p-wcnss")) continue;
                hide_from_linux(n);
                hidden++;
            }
        /* the fuel gauge names the charger as its power supply, and Linux holds a device
         * back until what it names has a driver - the charger, kept, never gets one: the
         * battery waited forever (0.22.6).  Its link goes nowhere instead; the gauge
         * driver works without a charger (QRT patch) */
        static const char *const gauges[] = { "qcom,pmi8994-fg", "qcom,pmi8996-fg", "qcom,pmi8998-fg", NULL };
        for (int i = 0; gauges[i]; i++)
            for (int n = fdt_find_compatible(-1, gauges[i]); n >= 0; n = fdt_find_compatible(n, gauges[i])) {
                int len;
                u8 *ps = (u8 *)fdt_prop(n, "power-supplies", &len);
                if (ps && len >= 4) { memset(ps, 0xff, (usize)len); klog("argon: fuel gauge: works without the charger"); }
            }
        klog("argon: %d device(s) kept from Linux (secure world, reset/power-off, thermal, charger, USB; qrt.allhw gives them)", hidden);
    }
    if (bootargs && strstr(bootargs, "qrt.nolinux")) klog("linux: off (qrt.nolinux)");
    else if (plog_last_boot_failed()) {
        klog("safe boot: the last two boots reset while Linux's drivers were starting (%d lines kept); Linux stays off this once",
             plog_prev_lines());
        logview_show_previous();
        plog_state(PLOG_LINUX_OK);           /* the next boot tries Linux again */
        void plog_clear_fails(void);
        plog_clear_fails();
    } else {
        int plog_last_boot_unclean(void);
        if (plog_last_boot_unclean()) {      /* a reset, a crash or the power key held: what came last */
            klog("plog: the last boot ended without a shutdown; its log is up (volume up x3 closes it)");
            logview_show_previous();
        }
        linux_start(dtb);
    }
    shell_main();
    panic("the shell returned");
}
