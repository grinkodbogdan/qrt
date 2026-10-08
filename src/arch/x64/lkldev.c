/*
 * lkldev.c - what a Linux driver host needs from Tessera (system calls 1040..1044).
 *
 * QRT runs Linux's own device drivers in a user program, /bin/linuxdrv: the Linux kernel
 * built as a library (LKL, the anykernel / LibOS approach) with QRT as its host.  Each
 * driver host owns one PCI device.  Tessera gives it what Linux's PCI core asks of a
 * platform, and nothing more:
 *
 *   qrt_pci_claim(bdf)                the device, if no Tessera driver has it
 *   qrt_pci_config(bdf, off, size, value*, write)   configuration space
 *   qrt_pci_map_bar(bdf, bar, size*)  a memory BAR mapped uncached into the process
 *   qrt_dma_alloc(bytes, phys*)       physically contiguous memory, mapped: LKL's RAM, so a
 *                                     driver's DMA address is its offset plus the base
 *   qrt_pci_info(bdf, info*)          vendor, device, class (for the System app's list)
 *   qrt_net_attach(mac*)              a network interface in Tessera's own stack for a
 *                                     Linux network driver: Linux only drives the card,
 *   qrt_net_rx(h, frame*, len)        Tessera does ARP, DHCP, TCP for its programs - the
 *   qrt_net_tx(h, buf*, cap, ms)      driver host passes Ethernet frames both ways
 *
 * Interrupts need no system call: like LKL's VFIO host, the driver host watches the
 * Interrupt Status bit of the PCI status register (legacy INTx) and raises Linux's IRQ.
 * bdf = bus << 8 | device << 3 | function.
 */
#include "proc.h"
#include "mm.h"
#include "../../drivers/pci.h"
#include "../../net/net.h"
#include "../../net/netstack.h"
#include "../../kernel/vfs.h"

enum { EPERM = 1, ENOENT = 2, EFAULT = 14, EBUSY = 16, EINVAL = 22, ENOMEM = 12 };
#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))

static struct { pci_dev_t *d; int pid; } claims[8];

static pci_dev_t *dev_of(u64 bdf) {
    for (int i = 0; i < pci_ndevs; i++) {
        pci_dev_t *d = &pci_devs[i];
        if (((u32)d->bus << 8 | (u32)d->dev << 3 | d->fn) == (u32)bdf) return d;
    }
    return NULL;
}
static pci_dev_t *claimed(proc_t *p, u64 bdf) {
    pci_dev_t *d = dev_of(bdf);
    for (int i = 0; d && i < 8; i++) if (claims[i].d == d && claims[i].pid == p->pid) return d;
    return NULL;
}

/* map [phys, phys + bytes) into the process; pages are not freed with the mapping */
static i64 map_phys(proc_t *p, u64 phys, u64 bytes, int uc) {
    bytes = (bytes + PAGE - 1) & ~(PAGE - 1);
    u64 fl = proc_vma_lock();
    u64 va = proc_find_free(p, bytes);
    int e = !va || proc_add_vma_prot(p, va, va + bytes, PROT_READ | PROT_WRITE, NULL, 0);
    proc_vma_unlock(fl);
    if (e) return -ENOMEM;
    for (u64 o = 0; o < bytes; o += PAGE) as_map(p->cr3, va + o, phys + o, AS_W | AS_SHARED | (uc ? AS_UC : 0));
    return (i64)va;
}

/* ---- network interfaces of Linux drivers ---- */
#define NQ 64
#define FMAX 1600
typedef struct { u16 len; u8 data[FMAX]; } lframe_t;
typedef struct { lframe_t f[NQ]; volatile int head, tail; volatile int lock; } fq_t;
static struct lnet { netif_t nif; int pid, used; fq_t rx, tx; char name[24]; } lnets[2];

static int fq_put(fq_t *q, const u8 *d, usize len) {
    if (len > FMAX) return -1;
    u64 fl = irq_save();
    while (__atomic_exchange_n(&q->lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    int next = (q->tail + 1) % NQ, ok = next != q->head;
    if (ok) { memcpy(q->f[q->tail].data, d, len); q->f[q->tail].len = (u16)len; q->tail = next; }
    __atomic_store_n(&q->lock, 0, __ATOMIC_RELEASE);
    irq_restore(fl);
    return ok ? 0 : -1;
}
static int fq_get(fq_t *q, u8 *d, usize cap) {
    u64 fl = irq_save();
    while (__atomic_exchange_n(&q->lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    int n = -1;
    if (q->head != q->tail) {
        n = q->f[q->head].len;
        if ((usize)n > cap) n = (int)cap;
        memcpy(d, q->f[q->head].data, (usize)n);
        q->head = (q->head + 1) % NQ;
    }
    __atomic_store_n(&q->lock, 0, __ATOMIC_RELEASE);
    irq_restore(fl);
    return n;
}
static int lnet_send(netif_t *n, const u8 *eth, usize len) {
    struct lnet *l = (struct lnet *)n;
    return fq_put(&l->tx, eth, len);
}

/* the shell's network poll (netstack.c), with the network lock held */
void lkl_net_poll(void) {
    static u8 buf[FMAX];
    for (int i = 0; i < 2; i++) {
        struct lnet *l = &lnets[i];
        if (!l->used) continue;
        if (l->nif.link && !proc_by_pid(l->pid)) {           /* the driver host is gone */
            l->nif.link = 0;
            net_link_changed(&l->nif);
            klog("linuxdrv: %s went away with its driver host", l->nif.name);
            continue;
        }
        int n;
        for (int k = 0; k < NQ && (n = fq_get(&l->rx, buf, sizeof buf)) > 0; k++) net_input(&l->nif, buf, (usize)n);
    }
}

/* ---- start a driver host for each device Linux should run (/etc/linuxdrv.conf) ---- */
static int conf_matches(const char *conf, const pci_dev_t *d) {
    for (const char *l = conf; *l; ) {
        const char *e = l;
        while (*e && *e != '\n') e++;
        char line[64];
        usize n = MIN((usize)(e - l), sizeof line - 1);
        memcpy(line, l, n); line[n] = 0;
        l = *e ? e + 1 : e;
        if (line[0] == '#' || !line[0]) continue;
        u32 v = 0, dv = 0; int i = 0, part = 0, digits = 0;
        for (; line[i] && line[i] != ' ' && line[i] != '\t'; i++) {
            char c = line[i];
            int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (c == ':') { part++; continue; }
            if (x < 0) break;
            if (part == 0) v = v << 4 | (u32)x; else dv = dv << 4 | (u32)x;
            digits++;
        }
        if (part == 1 && digits && v == d->vendor && dv == d->device) return 1;
    }
    return 0;
}

void linuxdrv_autostart(void) {
    vnode_t *c = vfs_lookup("/etc/linuxdrv.conf"), *b = vfs_lookup("/bin/linuxdrv");
    if (!c || !c->data || !b) return;
    char conf[4096];
    usize n = MIN((usize)c->size, sizeof conf - 1);
    memcpy(conf, c->data, n); conf[n] = 0;
    int started = 0;
    for (int i = 0; i < pci_ndevs && started < 2; i++) {
        pci_dev_t *d = &pci_devs[i];
        if (d->driver || !conf_matches(conf, d)) continue;
        char bdf[16], err[96];
        fmt(bdf, sizeof bdf, "%02x:%02x.%x", d->bus, d->dev, d->fn);
        const char *argv[] = { "linuxdrv", bdf };
        proc_t *p = proc_spawn("/bin/linuxdrv", 2, argv, NULL, err, sizeof err);
        klog("linuxdrv: %04x:%04x at %s: %s", d->vendor, d->device, bdf, p ? "Linux's driver starts (driver host)" : err);
        if (p) started++;
    }
}

i64 lkl_call(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    switch (nr) {
    case 1040: {                                               /* claim */
        pci_dev_t *d = dev_of(a0);
        if (!d) return -ENOENT;
        if (d->driver && strcmp(d->driver, "linux")) return -EBUSY;   /* a Tessera driver runs it */
        int slot = -1;
        for (int i = 0; i < 8; i++) {
            if (claims[i].d == d && claims[i].pid != p->pid && proc_by_pid(claims[i].pid)) return -EBUSY;
            if (claims[i].d == d || (!claims[i].d && slot < 0)) slot = i;
        }
        if (slot < 0) return -ENOMEM;
        claims[slot].d = d; claims[slot].pid = p->pid;
        d->driver = "linux";
        u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
        pci_write32(d->bus, d->dev, d->fn, 4, (cmd & ~0x400u) | 0x6);   /* memory, bus master, INTx on */
        klog("linuxdrv: %s (pid %d) drives %02x:%02x.%x (%04x:%04x)", p->name, p->pid, d->bus, d->dev, d->fn, d->vendor, d->device);
        return 0;
    }
    case 1041: {                                               /* config space */
        pci_dev_t *d = claimed(p, a0);
        u32 off = (u32)a1, size = (u32)a2;
        if (!d) return -EPERM;
        if ((size != 1 && size != 2 && size != 4) || off > 0xfff || (off & (size - 1)) || !UOK(a3, 4)) return -EINVAL;
        u32 *val = (u32 *)(usize)a3, sh = (off & 3) * 8, mask = size == 4 ? ~0u : ((1u << (size * 8)) - 1) << sh;
        u32 cur = pci_read32(d->bus, d->dev, d->fn, (u16)(off & ~3u));
        if (!a4) { *val = (cur & mask) >> sh; return (i64)size; }
        pci_write32(d->bus, d->dev, d->fn, (u16)(off & ~3u), (cur & ~mask) | ((*val << sh) & mask));
        return (i64)size;
    }
    case 1042: {                                               /* map a memory BAR */
        pci_dev_t *d = claimed(p, a0);
        int bar = (int)a1;
        if (!d) return -EPERM;
        if (bar < 0 || bar > 5 || !UOK(a2, 8)) return -EINVAL;
        u16 reg = (u16)(0x10 + bar * 4);
        u32 lo = pci_read32(d->bus, d->dev, d->fn, reg);
        if (lo & 1) return -EINVAL;                            /* an I/O port BAR */
        int is64 = ((lo >> 1) & 3) == 2;
        u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
        pci_write32(d->bus, d->dev, d->fn, 4, cmd & ~0x2u);    /* size it with decoding off */
        pci_write32(d->bus, d->dev, d->fn, reg, ~0u);
        u64 sz = pci_read32(d->bus, d->dev, d->fn, reg) & ~0xfu;
        pci_write32(d->bus, d->dev, d->fn, reg, lo);
        if (is64) {
            u32 hi = pci_read32(d->bus, d->dev, d->fn, (u16)(reg + 4));
            pci_write32(d->bus, d->dev, d->fn, (u16)(reg + 4), ~0u);
            sz |= (u64)pci_read32(d->bus, d->dev, d->fn, (u16)(reg + 4)) << 32;
            pci_write32(d->bus, d->dev, d->fn, (u16)(reg + 4), hi);
        } else sz |= 0xffffffff00000000ull;
        pci_write32(d->bus, d->dev, d->fn, 4, cmd);
        u64 size = ~sz + 1, base = pci_bar(d->bus, d->dev, d->fn, bar);
        if (!base || !size || size > (1ull << 30)) return -ENOENT;
        i64 va = map_phys(p, base, size, 1);
        if (va > 0) *(u64 *)(usize)a2 = size;
        return va;
    }
    case 1043: {                                               /* contiguous DMA memory */
        u64 bytes = (a0 + PAGE - 1) & ~(PAGE - 1);
        if (!bytes || bytes > (512ull << 20) || !UOK(a1, 8)) return -EINVAL;
        if (bytes > pmm_free_bytes() / 2) return -ENOMEM;
        u64 phys = pmm_alloc_contig(bytes / PAGE);
        if (!phys) return -ENOMEM;
        i64 va = map_phys(p, phys, bytes, 0);
        if (va > 0) *(u64 *)(usize)a1 = phys;
        return va;
    }
    case 1045: {                                               /* net_attach(mac*) */
        if (!UOK(a0, 6)) return -EFAULT;
        int h = -1;
        for (int i = 0; i < 2 && h < 0; i++) if (lnets[i].used && lnets[i].pid == p->pid) h = i;
        for (int i = 0; i < 2 && h < 0; i++) if (!lnets[i].used || !proc_by_pid(lnets[i].pid)) h = i;
        if (h < 0) return -EBUSY;
        struct lnet *l = &lnets[h];
        net_lock();
        int fresh = !l->used;
        l->pid = p->pid;
        l->rx.head = l->rx.tail = l->tx.head = l->tx.tail = 0;
        memcpy(l->nif.mac, (const void *)(usize)a0, 6);
        strlcpy(l->name, "Ethernet (Linux)", sizeof l->name);
        l->nif.name = l->name;
        l->nif.send = lnet_send;
        if (fresh) net_register(&l->nif);
        l->used = 1;
        l->nif.link = 1;
        net_link_changed(&l->nif);
        net_unlock();
        klog("linuxdrv: %s (pid %d): network interface %02x:%02x:%02x:%02x:%02x:%02x in Tessera's stack", p->name, p->pid,
             l->nif.mac[0], l->nif.mac[1], l->nif.mac[2], l->nif.mac[3], l->nif.mac[4], l->nif.mac[5]);
        return h;
    }
    case 1046: {                                               /* net_rx(h, frame*, len): Linux received it */
        if (a0 >= 2 || !lnets[a0].used || lnets[a0].pid != p->pid) return -EINVAL;
        if (!a2 || a2 > FMAX || !UOK(a1, a2)) return -EFAULT;
        return fq_put(&lnets[a0].rx, (const u8 *)(usize)a1, a2) ? -ENOMEM : 0;
    }
    case 1047: {                                               /* net_tx(h, buf*, cap, ms): a frame to send, or 0 */
        if (a0 >= 2 || !lnets[a0].used || lnets[a0].pid != p->pid) return -EINVAL;
        if (!UOK(a1, a2)) return -EFAULT;
        u64 end = k_now_ms() + a3;
        static u8 tmp[FMAX];
        for (;;) {
            int n = fq_get(&lnets[a0].tx, tmp, sizeof tmp);
            if (n > 0) { usize c = MIN((usize)n, (usize)a2); memcpy((void *)(usize)a1, tmp, c); return (i64)c; }
            if (k_now_ms() >= end || proc_interrupted(p)) return 0;
            thread_sleep_ms(1);
        }
    }
    case 1044: {                                               /* info: vendor, device, class */
        pci_dev_t *d = dev_of(a0);
        if (!d) return -ENOENT;
        if (!UOK(a1, 8)) return -EFAULT;
        u32 *o = (u32 *)(usize)a1;
        o[0] = (u32)d->vendor << 16 | d->device;
        o[1] = (u32)d->class_code << 16 | (u32)d->subclass << 8 | d->prog_if;
        return 0;
    }
    }
    return -38;
}
