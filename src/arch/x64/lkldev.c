/*
 * lkldev.c - what a Linux driver host needs from Tessera (system calls 1040..1057).
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
 *   qrt_pci_report(bdf, driver*)      which Linux driver took the device; NULL gives it back
 *   qrt_fs_attach(name*, what*)       a disk Linux mounted, as /mnt/<name> in Tessera's VFS:
 *   qrt_fs_serve(req*, buf*, cap, ms) the driver host answers the VFS's requests (list,
 *   qrt_fs_reply(id, result, buf*, n) read, write, create, unlink, rename) with Linux's
 *   qrt_fs_detach(h)                  file systems
 *   qrt_input(kind, a, b, c, d)       keys (HID usage) and pointer reports from Linux's input
 *                                     drivers (evdev), into the shell's event stream
 *   qrt_snd_attach(name*, rate, ch)   a sound output played by a Linux sound driver (ALSA):
 *   qrt_snd_pull(h, buf*, frames, ms) the driver host pulls Tessera's mix
 *   qrt_dma_addr(va, len)             the physical address of other memory of the process
 *                                     (a buffer on a thread's stack), if it is contiguous
 *
 * Every PCI device no Tessera driver runs goes to one driver host at boot (graphics and
 * bridges aside); /etc/linuxdrv.conf can keep a device away from Linux or give Linux one
 * Tessera has a driver for.
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
#include "../../kernel/dev.h"
#include "../../kernel/sound.h"
#include "../../drivers/usb/xhci.h"

enum { EPERM = 1, ENOENT = 2, EFAULT = 14, EBUSY = 16, EINVAL = 22, ENOMEM = 12 };
#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))

#define NCLAIM 32
static struct { pci_dev_t *d; int pid; u32 cmd; } claims[NCLAIM];

static pci_dev_t *dev_of(u64 bdf) {
    for (int i = 0; i < pci_ndevs; i++) {
        pci_dev_t *d = &pci_devs[i];
        if (((u32)d->bus << 8 | (u32)d->dev << 3 | d->fn) == (u32)bdf) return d;
    }
    return NULL;
}
static pci_dev_t *claimed(proc_t *p, u64 bdf) {
    pci_dev_t *d = dev_of(bdf);
    for (int i = 0; d && i < NCLAIM; i++) if (claims[i].d == d && claims[i].pid == p->pid) return d;
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
#define NNET 4
static struct lnet { netif_t nif; int pid, used; fq_t rx, tx; char name[40]; u8 txb[FMAX]; } lnets[NNET];

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
static void fs_reap(void);
static void snd_reap(void);
void lkl_net_poll(void) {
    static u8 buf[FMAX];
    fs_reap();
    snd_reap();
    for (int i = 0; i < NNET; i++) {
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

/* ---- which devices Linux runs (/etc/linuxdrv.conf: "never|always vendor:device") ---- */
static int conf_has(const char *word, const pci_dev_t *d) {
    vnode_t *c = vfs_lookup("/etc/linuxdrv.conf");
    if (!c || !c->data) return 0;
    usize wl = strlen(word);
    for (u64 i = 0; i < c->size; ) {
        const char *l = (const char *)c->data + i;
        u64 e = i;
        while (e < c->size && c->data[e] != '\n') e++;
        usize n = (usize)(e - i);
        i = e + 1;
        if (n <= wl || memcmp(l, word, wl) || (l[wl] != ' ' && l[wl] != '\t')) continue;
        u32 v = 0, dv = 0; int part = 0, digits = 0;
        for (usize j = wl; j < n; j++) {
            char ch = l[j];
            int x = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (ch == ' ' || ch == '\t') { if (digits) break; continue; }
            if (ch == ':') { part++; continue; }
            if (x < 0) break;
            if (part == 0) v = v << 4 | (u32)x; else dv = dv << 4 | (u32)x;
            digits++;
        }
        if (part == 1 && digits && v == d->vendor && (dv == d->device || dv == 0xffff)) return 1;
    }
    return 0;
}
/* dev.c: a device Linux should run although Tessera has a driver ("always") */
int linuxdrv_preferred(const pci_dev_t *d) { return k.native && vfs_lookup("/bin/linuxdrv") && conf_has("always", d); }

static int linux_candidate(const pci_dev_t *d) {
    if (d->driver) return 0;                                      /* Tessera runs it */
    if (d->class_code == 0x06 || d->class_code == 0x03) return 0; /* bridges; the display (GOP framebuffer) */
    if (d->class_code == 0x05) return 0;                          /* memory controllers: nothing to drive */
    /* the rest - SMBus and other port-I/O devices included (0.20.0), system peripherals,
     * signal processing - goes to Linux's drivers when Tessera has none */
    return !conf_has("never", d);
}

static device_t *devof(const pci_dev_t *p) {
    for (int i = 0; i < n_devs; i++) if (devs[i].pci == p) return &devs[i];
    return NULL;
}

void linuxdrv_autostart(void) {
    if (!vfs_lookup("/bin/linuxdrv")) return;
    const char *argv[2 + NCLAIM];
    static char bdfs[NCLAIM][12];
    int argc = 0;
    argv[argc++] = "linuxdrv";
    for (int i = 0; i < pci_ndevs && argc < 1 + NCLAIM; i++) {
        pci_dev_t *d = &pci_devs[i];
        if (!linux_candidate(d)) continue;
        fmt(bdfs[argc - 1], sizeof bdfs[0], "%02x:%02x.%x", d->bus, d->dev, d->fn);
        argv[argc] = bdfs[argc - 1];
        argc++;
        device_t *dv = devof(d);
        if (dv) strlcpy(dv->status, "offered to Linux's drivers (driver host)", sizeof dv->status);
    }
    if (argc == 1) return;
    char err[96];
    proc_t *p = proc_spawn("/bin/linuxdrv", argc, argv, NULL, err, sizeof err);
    klog("linuxdrv: %d device%s offered to Linux's drivers: %s", argc - 1, argc == 2 ? "" : "s", p ? "driver host starts" : err);
}

/* ---- disks Linux mounted, served into the VFS ---- */
#define NMNT 16
#define NREQ 16
static struct rmnt { int pid, used; char name[32], what[64]; vnode_t *root; } mnts[NMNT];
typedef struct { u32 id, mnt, op, pad; u64 off, len; char path[512], path2[512]; } fsreq_t;   /* what the server sees */
static struct rreq { volatile int state; fsreq_t r; int pid; void *kbuf; volatile i64 result; } reqs[NREQ];
enum { RQ_FREE, RQ_QUEUED, RQ_TAKEN, RQ_DONE };
static volatile int rlock;
static u32 rseq;
static u64 rl_take(void) { u64 f = irq_save(); while (__atomic_exchange_n(&rlock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause"); return f; }
static void rl_give(u64 f) { __atomic_store_n(&rlock, 0, __ATOMIC_RELEASE); irq_restore(f); }

static i64 remote_call(void *mnt, int op, const char *path, const char *path2, u64 off, void *buf, u64 len) {
    struct rmnt *m = mnt;
    int h = (int)(m - mnts), pid = m->pid;
    if (!m->used || !proc_by_pid(pid)) return -5;                  /* EIO: the driver host is gone */
    struct rreq *q = NULL;
    for (;;) {
        u64 f = rl_take();
        for (int i = 0; i < NREQ && !q; i++) if (reqs[i].state == RQ_FREE) { q = &reqs[i]; q->state = RQ_TAKEN; }
        rl_give(f);
        if (q) break;
        if (!proc_by_pid(pid)) return -5;
        thread_sleep_ms(1);
    }
    memset(&q->r, 0, sizeof q->r);
    q->r.id = ++rseq; q->r.mnt = (u32)h; q->r.op = (u32)op; q->r.off = off; q->r.len = len;
    strlcpy(q->r.path, path, sizeof q->r.path);
    if (path2) strlcpy(q->r.path2, path2, sizeof q->r.path2);
    q->pid = pid; q->kbuf = buf; q->result = -5;
    __atomic_store_n(&q->state, RQ_QUEUED, __ATOMIC_RELEASE);
    for (int spin = 0; q->state != RQ_DONE; spin++) {
        if (!proc_by_pid(pid)) {                                   /* died: whoever holds it, it is ours again */
            u64 f = rl_take();
            q->state = RQ_FREE;
            rl_give(f);
            return -5;
        }
        if (spin < 50) __asm__ volatile("pause"); else thread_sleep_ms(1);
    }
    i64 r = q->result;
    __atomic_store_n(&q->state, RQ_FREE, __ATOMIC_RELEASE);
    return r;
}

/* the driver host went away: its disks leave /mnt */
static void fs_reap(void) {
    for (int i = 0; i < NMNT; i++)
        if (mnts[i].used && !proc_by_pid(mnts[i].pid)) {
            vfs_unmount_remote(mnts[i].root);
            mnts[i].used = 0;
            klog("linuxdrv: /mnt/%s went away with its driver host", mnts[i].name);
        }
}

/* ---- input from Linux's drivers ---- */
static event_t inq[64];
static volatile int inq_n, inlock;
int lkl_input_poll(event_t *out, int max) {                        /* hal.c, the UI thread */
    if (!inq_n) return 0;
    u64 f = irq_save();
    while (__atomic_exchange_n(&inlock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    int n = MIN(inq_n, max);
    memcpy(out, inq, (usize)n * sizeof *out);
    memmove(inq, inq + n, (usize)(inq_n - n) * sizeof *inq);
    inq_n -= n;
    __atomic_store_n(&inlock, 0, __ATOMIC_RELEASE);
    irq_restore(f);
    return n;
}
static void inq_put(event_t e) {
    u64 f = irq_save();
    while (__atomic_exchange_n(&inlock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    event_t *l = inq_n ? &inq[inq_n - 1] : NULL;                   /* movements between two polls add up */
    if (l && e.type == EV_REL && l->type == EV_REL && l->scan == e.scan && l->from_mouse == e.from_mouse && !l->dy && !e.dy) {
        if (e.from_mouse == 2) { l->x = e.x; l->y = e.y; } else { l->x += e.x; l->y += e.y; }
    } else if (inq_n < (int)ARRAY_LEN(inq)) inq[inq_n++] = e;
    __atomic_store_n(&inlock, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

/* ---- sound outputs played by Linux's drivers ---- */
#define NSND 2
#define SRING 4096                                                  /* frames */
static struct lsnd { snd_output_t out; int pid, used, on; char name[64]; i16 ring[SRING * 2]; volatile u32 head, tail; } lsnds[NSND];
static void lsnd_pump(snd_output_t *o) {                            /* the sound thread: keep the ring full */
    struct lsnd *l = (struct lsnd *)o;
    int ch = o->channels;
    for (;;) {
        u32 used = l->tail - l->head, frames = 256;
        if (used + frames > SRING / 2) break;                      /* about 40 ms ahead at 48 kHz */
        u32 at = l->tail % SRING;
        if (at + frames > SRING) frames = SRING - at;
        snd_mix(l->ring + at * (u32)ch, (int)frames, o->rate, ch);
        __atomic_store_n(&l->tail, l->tail + frames, __ATOMIC_RELEASE);
    }
}
static void snd_reap(void) {
    for (int i = 0; i < NSND; i++)
        if (lsnds[i].on && !proc_by_pid(lsnds[i].pid)) {
            snd_output_remove(&lsnds[i].out);
            lsnds[i].on = lsnds[i].used = 0;
            klog("linuxdrv: sound output %s went away with its driver host", lsnds[i].name);
        }
}

/* the System app: disks and devices Linux runs */
int linuxdrv_mounts(char *out, int cap) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < NMNT; i++)
        if (mnts[i].used && proc_by_pid(mnts[i].pid) && n < cap - 1)
            n += fmt(out + n, (usize)(cap - n), "%s/mnt/%s (%s)", n ? ", " : "", mnts[i].name, mnts[i].what);
    return MIN(n, cap - 1);
}

i64 lkl_call(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    switch (nr) {
    case 1057: {                                               /* ioports: in/out for Linux's drivers */
        /* only a driver host that holds a device: Linux's drivers for port-I/O devices
         * (legacy BARs, SMBus, old NICs and sound) reach the ports from user mode */
        int holds = 0;
        for (int i = 0; i < NCLAIM && !holds; i++) holds = claims[i].d && claims[i].pid == p->pid;
        if (!holds) return -1;                                 /* EPERM */
        p->ioports = 1;
        klog("linuxdrv: x86 I/O ports granted to %s (pid %d)", p->name, p->pid);
        return 0;
    }
    case 1040: {                                               /* claim */
        pci_dev_t *d = dev_of(a0);
        if (!d) return -ENOENT;
        if (d->driver && strcmp(d->driver, "linux")) return -EBUSY;   /* a Tessera driver runs it */
        int slot = -1;
        for (int i = 0; i < NCLAIM; i++) {
            if (claims[i].d == d && claims[i].pid != p->pid && proc_by_pid(claims[i].pid)) return -EBUSY;
            if (claims[i].d == d || (!claims[i].d && slot < 0)) slot = i;
        }
        if (slot < 0) return -ENOMEM;
        u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
        if (claims[slot].d != d) claims[slot].cmd = cmd & 0xffff;
        claims[slot].d = d; claims[slot].pid = p->pid;
        d->driver = "linux";
        pci_write32(d->bus, d->dev, d->fn, 4, (cmd & ~0x400u) | 0x6);   /* memory, bus master, INTx on */
        return 0;
    }
    case 1048: {                                               /* report(bdf, linux driver name or NULL) */
        pci_dev_t *d = claimed(p, a0);
        if (!d) return -EPERM;
        device_t *dv = devof(d);
        if (!a1) {                                             /* no Linux driver: give it back as it was */
            for (int i = 0; i < NCLAIM; i++)
                if (claims[i].d == d) {
                    u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
                    pci_write32(d->bus, d->dev, d->fn, 4, (cmd & 0xffff0000u) | claims[i].cmd);
                    claims[i].d = NULL;
                }
            d->driver = NULL;
            if (dv) strlcpy(dv->status, "no driver in Tessera or Linux", sizeof dv->status);
            return 0;
        }
        char name[32];
        if (!UOK(a1, 1)) return -EFAULT;
        strlcpy(name, (const char *)(usize)a1, sizeof name);
        if (dv) fmt(dv->status, sizeof dv->status, "Linux driver %s (driver host)", name);
        klog("linuxdrv: %02x:%02x.%x (%04x:%04x) runs with Linux's %s", d->bus, d->dev, d->fn, d->vendor, d->device, name);
        return 0;
    }
    case 1049: {                                               /* fs_attach(name*, what*) */
        if (!UOK(a0, 1) || !UOK(a1, 1)) return -EFAULT;
        fs_reap();
        int h = -1;
        for (int i = 0; i < NMNT && h < 0; i++) if (!mnts[i].used) h = i;
        if (h < 0) return -ENOMEM;
        struct rmnt *m = &mnts[h];
        strlcpy(m->name, (const char *)(usize)a0, sizeof m->name);
        strlcpy(m->what, (const char *)(usize)a1, sizeof m->what);
        for (char *c = m->name; *c; c++) if (*c == '/') *c = '_';
        char path[48];
        fmt(path, sizeof path, "/mnt/%s", m->name);
        if (vfs_lookup(path)) return -EBUSY;
        vfs_remote_call = remote_call;
        m->pid = p->pid;
        m->used = 1;
        m->root = vfs_mount_remote(path, m);
        if (!m->root) { m->used = 0; return -EBUSY; }
        klog("linuxdrv: %s (pid %d): %s at %s", p->name, p->pid, m->what, path);
        return h;
    }
    case 1050: {                                               /* fs_serve(req*, buf*, cap, ms) */
        if (!UOK(a0, sizeof(fsreq_t)) || !UOK(a1, a2)) return -EFAULT;
        u64 end = k_now_ms() + a3;
        for (;;) {
            struct rreq *q = NULL;
            u64 f = rl_take();
            for (int i = 0; i < NREQ && !q; i++)
                if (reqs[i].state == RQ_QUEUED && reqs[i].pid == p->pid) { q = &reqs[i]; q->state = RQ_TAKEN; }
            rl_give(f);
            if (q) {
                if (q->r.op == VR_WRITE) {
                    if (q->r.len > a2) q->r.len = a2;
                    memcpy((void *)(usize)a1, q->kbuf, (usize)q->r.len);
                }
                memcpy((void *)(usize)a0, &q->r, sizeof q->r);
                return 1;
            }
            if (k_now_ms() >= end || proc_interrupted(p)) return 0;
            thread_sleep_ms(1);
        }
    }
    case 1051: {                                               /* fs_reply(id, result, buf*, n) */
        for (int i = 0; i < NREQ; i++) {
            struct rreq *q = &reqs[i];
            if (q->state != RQ_TAKEN || q->pid != p->pid || q->r.id != (u32)a0) continue;
            i64 r = (i64)a1;
            if (r > 0 && (q->r.op == VR_READ || q->r.op == VR_LIST)) {
                if ((u64)r > q->r.len) r = (i64)q->r.len;
                if (!UOK(a2, r)) r = -EFAULT;
                else memcpy(q->kbuf, (const void *)(usize)a2, (usize)r);
            }
            q->result = r;
            __atomic_store_n(&q->state, RQ_DONE, __ATOMIC_RELEASE);
            return 0;
        }
        return -ENOENT;
    }
    case 1053: {                                               /* input(kind, a, b, c, d) */
        event_t e = { 0 };
        if (a0 == 0) {                                         /* a key pressed: HID usage, shift, ctrl */
            usb_key_of((u8)a1, (int)a2, (int)a3, &e.scan, &e.ch);
            if (!e.scan && !e.ch) return 0;
            e.type = EV_KEY;
        } else if (a0 == 1 || a0 == 2) {                      /* pointer: a1 = x | y << 32, a2 wheel, a3 buttons */
            e.type = EV_REL;
            e.x = (i32)(u32)a1; e.y = (i32)(u32)(a1 >> 32);
            e.dy = (int)(i32)(u32)a2;
            e.scan = (u16)a3;
            e.from_mouse = a0 == 2 ? 2 : 1;                    /* 2: absolute, 0..65535 (touch, tablets) */
        } else return -EINVAL;
        inq_put(e);
        return 0;
    }
    case 1054: {                                               /* snd_attach(name*, rate, channels) */
        if (!UOK(a0, 1) || a1 < 8000 || a1 > 192000 || (a2 != 1 && a2 != 2)) return -EINVAL;
        snd_reap();
        int h = -1;
        for (int i = 0; i < NSND && h < 0; i++) if (!lsnds[i].used) h = i;
        if (h < 0) return -EBUSY;
        struct lsnd *l = &lsnds[h];
        l->used = 1; l->pid = p->pid; l->head = l->tail = 0;
        strlcpy(l->name, (const char *)(usize)a0, sizeof l->name);
        l->out = (snd_output_t){ l->name, (int)a1, (int)a2, lsnd_pump, NULL };
        snd_output_add(&l->out);
        l->on = 1;
        klog("linuxdrv: %s (pid %d): sound output %s, %d Hz", p->name, p->pid, l->name, (int)a1);
        return h;
    }
    case 1055: {                                               /* snd_pull(h, buf*, frames, ms) */
        if (a0 >= NSND || !lsnds[a0].on || lsnds[a0].pid != p->pid) return -EINVAL;
        struct lsnd *l = &lsnds[a0];
        u32 ch = (u32)l->out.channels;
        if (!UOK(a1, a2 * ch * 2)) return -EFAULT;
        u64 end = k_now_ms() + a3;
        while (l->tail == l->head) {
            if (k_now_ms() >= end || proc_interrupted(p)) return 0;
            thread_sleep_ms(1);
        }
        u32 n = 0;
        while (n < a2 && l->head != l->tail) {
            u32 at = l->head % SRING, c = MIN(l->tail - l->head, SRING - at);
            c = MIN(c, (u32)a2 - n);
            memcpy((i16 *)(usize)a1 + n * ch, l->ring + at * ch, c * ch * 2);
            __atomic_store_n(&l->head, l->head + c, __ATOMIC_RELEASE);
            n += c;
        }
        return n;
    }
    case 1056: {                                               /* dma_addr(va, len): DMA outside Linux's RAM */
        if (!a1 || a1 > (1u << 20) || !UOK(a0, a1)) return -EFAULT;
        u64 pa = as_translate(p->cr3, a0);
        if (!pa) return -EFAULT;
        for (u64 v = (a0 & ~(PAGE - 1)) + PAGE; v < a0 + a1; v += PAGE)
            if (as_translate(p->cr3, v) != pa + (v - a0)) return -EINVAL;   /* pages not contiguous */
        return (i64)pa;
    }
    case 1052: {                                               /* fs_detach(h) */
        if (a0 >= NMNT || !mnts[a0].used || mnts[a0].pid != p->pid) return -EINVAL;
        vfs_unmount_remote(mnts[a0].root);
        mnts[a0].used = 0;
        klog("linuxdrv: /mnt/%s removed", mnts[a0].name);
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
    case 1045: {                                               /* net_attach(mac*, driver*) */
        if (!UOK(a0, 6)) return -EFAULT;
        int h = -1;
        for (int i = 0; i < NNET && h < 0; i++) if (lnets[i].used && lnets[i].pid == p->pid && !memcmp(lnets[i].nif.mac, (const void *)(usize)a0, 6)) h = i;
        for (int i = 0; i < NNET && h < 0; i++) if (!lnets[i].used || !proc_by_pid(lnets[i].pid)) h = i;
        if (h < 0) return -EBUSY;
        struct lnet *l = &lnets[h];
        net_lock();
        int fresh = !l->used;
        l->pid = p->pid;
        l->rx.head = l->rx.tail = l->tx.head = l->tx.tail = 0;
        memcpy(l->nif.mac, (const void *)(usize)a0, 6);
        if (a1 && UOK(a1, 1)) fmt(l->name, sizeof l->name, "Ethernet (Linux %s)", (const char *)(usize)a1);
        else strlcpy(l->name, "Ethernet (Linux)", sizeof l->name);
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
        if (a0 >= NNET || !lnets[a0].used || lnets[a0].pid != p->pid) return -EINVAL;
        if (!a2 || a2 > FMAX || !UOK(a1, a2)) return -EFAULT;
        return fq_put(&lnets[a0].rx, (const u8 *)(usize)a1, a2) ? -ENOMEM : 0;
    }
    case 1047: {                                               /* net_tx(h, buf*, cap, ms): a frame to send, or 0 */
        if (a0 >= NNET || !lnets[a0].used || lnets[a0].pid != p->pid) return -EINVAL;
        if (!UOK(a1, a2)) return -EFAULT;
        u64 end = k_now_ms() + a3;
        u8 *tmp = lnets[a0].txb;                               /* one tx thread per interface */
        for (;;) {
            int n = fq_get(&lnets[a0].tx, tmp, FMAX);
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
