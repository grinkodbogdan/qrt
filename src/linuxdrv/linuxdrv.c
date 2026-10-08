/*
 * linuxdrv - Linux's device drivers on QRT: the Linux kernel as a library (LKL, the
 * anykernel / LibOS approach) in a user program, with Tessera as its host.
 *
 *   linuxdrv [--test host:port] <bus:dev.fn>... [-- Linux kernel arguments]
 *
 * Tessera keeps its own personality - the shell, the native and Linux system calls, the
 * scheduler - and gives every PCI device it has no driver for to one driver host at boot
 * (src/arch/x64/lkldev.c).  The host runs them with the unmodified Linux drivers, inside a
 * complete Linux kernel (scheduler, memory manager, block layer, file systems, network
 * stack, USB core, ALSA, input core, sysfs) that runs as threads of this process, and
 * bridges what they make into Tessera:
 *
 *   network cards     every Ethernet interface: frames to and from an interface in
 *                     Tessera's own stack (ARP, DHCP, TCP stay Tessera's)
 *   disks             SATA, NVMe, SD, USB disks: each partition Linux can mount (ext4, FAT,
 *                     exFAT, NTFS (read-only), XFS, Btrfs, F2FS, ISO 9660, UDF) appears as
 *                     /mnt/<name> in Tessera's file tree; this host serves its requests
 *   keyboards, mice,  evdev: keys, pointer movement and touch into the shell's events
 *   touch screens
 *   sound cards       the first ALSA playback device plays Tessera's mix (a sound output)
 *   firmware          drivers' request_firmware() is answered from Tessera's /lib/firmware
 *
 * Devices no Linux driver takes are given back to Tessera.  With a USB controller given
 * to Linux (/etc/linuxdrv.conf: "always <vendor>:<device>"), every USB device works the
 * way it does on Linux.
 *
 * This file is also LKL's "host": the operations LKL asks of an operating system
 * (lkl_host.h), implemented on Tessera:
 *
 *   threads, semaphores, mutexes, TLS   pthreads (Tessera's clone/futex)
 *   timers                              a thread per timer, condition variables
 *   RAM                                 physically contiguous memory (system call 1043),
 *                                       so a driver's DMA address is its RAM offset plus
 *                                       the physical base - no IOMMU needed
 *   PCI                                 claim (1040), configuration space (1041), memory
 *                                       BARs mapped uncached (1042); interrupts: the PCI
 *                                       status register's Interrupt Status bit, polled
 *                                       every millisecond, as LKL's VFIO host does
 *
 * --test host:port instead gives the network card to Linux's own TCP/IP (DHCP at boot),
 * fetches http://host:port/ through it and prints the status line.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <setjmp.h>
#include <fcntl.h>
#include <signal.h>
#include <lkl.h>
#include <lkl_host.h>
#include "lkl/iomem.h"

/* ---- Tessera's driver-host system calls (src/arch/x64/lkldev.c) ---- */
#define SYS_PCI_CLAIM  1040
#define SYS_PCI_CONFIG 1041
#define SYS_PCI_MAPBAR 1042
#define SYS_DMA_ALLOC  1043
#define SYS_DMA_ADDR   1056
#define SYS_IOPORTS    1057

/* ---- x86 I/O ports: Linux's inb()/outb() and port BARs (LKL_HOST_IOPORT) arrive here
 * as addresses LKL_PCI_IOBASE + port; Tessera grants the ports once a device is claimed */
static int ports_granted;
static int port_io(unsigned long port, void *v, int size, int write) {
    if (!ports_granted) {
        long e = syscall(SYS_IOPORTS);
        printf("linuxdrv: x86 I/O ports for Linux's drivers: %s\n", e < 0 ? "refused" : "granted");
        if (e < 0) return -1;
        ports_granted = 1;
    }
    unsigned short pt = (unsigned short)port;
    if (write) {
        if (size == 1) __asm__ volatile("outb %0, %1" : : "a"(*(unsigned char *)v), "Nd"(pt));
        else if (size == 2) __asm__ volatile("outw %0, %1" : : "a"(*(unsigned short *)v), "Nd"(pt));
        else if (size == 4) __asm__ volatile("outl %0, %1" : : "a"(*(unsigned *)v), "Nd"(pt));
        else return -1;
    } else {
        if (size == 1) { unsigned char x; __asm__ volatile("inb %1, %0" : "=a"(x) : "Nd"(pt)); *(unsigned char *)v = x; }
        else if (size == 2) { unsigned short x; __asm__ volatile("inw %1, %0" : "=a"(x) : "Nd"(pt)); *(unsigned short *)v = x; }
        else if (size == 4) { unsigned x; __asm__ volatile("inl %1, %0" : "=a"(x) : "Nd"(pt)); *(unsigned *)v = x; }
        else return -1;
    }
    return 0;
}
static int qrt_iomem_access(const volatile void *addr, void *val, int size, int write) {
    unsigned long a = (unsigned long)addr;
    if (a >= LKL_PCI_IOBASE && a < LKL_PCI_IOBASE + LKL_PCI_IOSIZE) return port_io(a - LKL_PCI_IOBASE, val, size, write);
    return lkl_iomem_access(addr, val, size, write);
}

static unsigned long ram_virt, ram_size;
static unsigned long long ram_phys;

static void out(const char *s, int len) { (void)!write(1, s, (size_t)len); }

/* ---- synchronisation ---- */
struct lkl_sem { pthread_mutex_t m; pthread_cond_t c; int count; };
struct lkl_mutex { pthread_mutex_t m; };
struct lkl_tls_key { pthread_key_t k; };

static struct lkl_sem *sem_alloc(int count) {
    struct lkl_sem *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->count = count;
    return s;
}
static void sem_free(struct lkl_sem *s) { pthread_cond_destroy(&s->c); pthread_mutex_destroy(&s->m); free(s); }
static void sem_up(struct lkl_sem *s) {
    pthread_mutex_lock(&s->m);
    if (++s->count > 0) pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}
static void sem_down(struct lkl_sem *s) {
    pthread_mutex_lock(&s->m);
    while (s->count <= 0) pthread_cond_wait(&s->c, &s->m);
    s->count--;
    pthread_mutex_unlock(&s->m);
}
static struct lkl_mutex *mutex_alloc(int recursive) {
    struct lkl_mutex *m = calloc(1, sizeof *m);
    pthread_mutexattr_t a;
    if (!m) return NULL;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
static void mutex_free(struct lkl_mutex *m) { pthread_mutex_destroy(&m->m); free(m); }
static void mutex_lock(struct lkl_mutex *m) { pthread_mutex_lock(&m->m); }
static void mutex_unlock(struct lkl_mutex *m) { pthread_mutex_unlock(&m->m); }

/* ---- threads ---- */
struct thread_arg { void (*fn)(void *); void *arg; };
static void *thread_start(void *p) { struct thread_arg a = *(struct thread_arg *)p; free(p); a.fn(a.arg); return NULL; }
static lkl_thread_t thread_create(void (*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t at;
    struct thread_arg *a = malloc(sizeof *a);
    if (!a) return 0;
    a->fn = fn; a->arg = arg;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    int e = pthread_create(&t, &at, thread_start, a);
    pthread_attr_destroy(&at);
    if (e) { free(a); return 0; }
    return (lkl_thread_t)t;
}
static void thread_detach(void) { pthread_detach(pthread_self()); }
static void thread_exit(void) { pthread_exit(NULL); }
static int thread_join(lkl_thread_t t) { return pthread_join((pthread_t)t, NULL) ? -1 : 0; }
static lkl_thread_t thread_self(void) { return (lkl_thread_t)pthread_self(); }
static int thread_equal(lkl_thread_t a, lkl_thread_t b) { return pthread_equal((pthread_t)a, (pthread_t)b); }
static struct lkl_tls_key *tls_alloc(void (*destructor)(void *)) {
    struct lkl_tls_key *k = malloc(sizeof *k);
    if (!k) return NULL;
    if (pthread_key_create(&k->k, destructor)) { free(k); return NULL; }
    return k;
}
static void tls_free(struct lkl_tls_key *k) { pthread_key_delete(k->k); free(k); }
static int tls_set(struct lkl_tls_key *k, void *d) { return pthread_setspecific(k->k, d) ? -1 : 0; }
static void *tls_get(struct lkl_tls_key *k) { return pthread_getspecific(k->k); }

/* ---- time, timers (a thread each: Tessera has no POSIX timers) ---- */
static unsigned long long time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
struct qtimer { void (*fn)(void); pthread_t th; pthread_mutex_t m; pthread_cond_t c; unsigned long long due; int armed, quit; };
static void *timer_thread(void *p) {
    struct qtimer *t = p;
    pthread_mutex_lock(&t->m);
    while (!t->quit) {
        if (!t->armed) { pthread_cond_wait(&t->c, &t->m); continue; }
        unsigned long long now = time_ns();
        if (now < t->due) {
            unsigned long long wait = t->due - now;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(wait / 1000000000ull);
            ts.tv_nsec += (long)(wait % 1000000000ull);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&t->c, &t->m, &ts);
            continue;
        }
        t->armed = 0;
        pthread_mutex_unlock(&t->m);
        t->fn();
        pthread_mutex_lock(&t->m);
    }
    pthread_mutex_unlock(&t->m);
    return NULL;
}
static void *timer_alloc(void (*fn)(void)) {
    struct qtimer *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    if (pthread_create(&t->th, NULL, timer_thread, t)) { free(t); return NULL; }
    return t;
}
static int timer_set_oneshot(void *p, unsigned long ns) {
    struct qtimer *t = p;
    pthread_mutex_lock(&t->m);
    t->due = time_ns() + ns;
    t->armed = 1;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return 0;
}
static void timer_free(void *p) {
    struct qtimer *t = p;
    pthread_mutex_lock(&t->m);
    t->quit = 1;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    if (!pthread_equal(pthread_self(), t->th)) pthread_join(t->th, NULL);
    else pthread_detach(t->th);
}

/* ---- memory ---- */
static void *mem_alloc(unsigned long n) { return malloc(n); }
static void *h_memcpy(void *d, const void *s, unsigned long n) { return memcpy(d, s, n); }
static void *h_memset(void *d, int c, unsigned long n) { return memset(d, c, n); }
static void *h_memmove(void *d, const void *s, unsigned long n) { return memmove(d, s, n); }
static void *page_alloc(unsigned long size) {                   /* the kernel's RAM: DMA-able */
    unsigned long long phys = 0;
    long va = syscall(SYS_DMA_ALLOC, size, &phys);
    if (va == -1 && errno == ENOSYS) {                          /* not on QRT (a Linux build machine): no DMA */
        void *p = aligned_alloc(4096, size);
        ram_virt = (unsigned long)p;
        return p;
    }
    if (va < 0) { fprintf(stderr, "linuxdrv: no %lu MB of contiguous memory (%ld)\n", size >> 20, va); return NULL; }
    ram_virt = (unsigned long)va;
    ram_size = size;
    ram_phys = phys;
    return (void *)va;
}
static void page_free(void *addr, unsigned long size) { (void)addr; (void)size; }
static void panic_host(void) { fprintf(stderr, "linuxdrv: Linux panicked\n"); _exit(70); }

static void jmp_buf_set(struct lkl_jmp_buf *j, void (*f)(void)) { if (!setjmp(*(jmp_buf *)j->buf)) f(); }
static void jmp_buf_longjmp(struct lkl_jmp_buf *j, int v) { longjmp(*(jmp_buf *)j->buf, v); }

/* ---- PCI ---- */
/* MSI and MSI-X: each vector's message is a DMA write of 1 to its own word of a page
 * this host watches (msi), so no interrupt controller is involved */
#define MAXVEC 32
struct lkl_pci_dev { unsigned bdf; int irq; volatile int quit; pthread_t th; volatile unsigned *msi; unsigned long long msi_phys; int msi_irq[MAXVEC]; volatile int nmsi; };
#define MAXDEV 32
static struct lkl_pci_dev *added[MAXDEV];                 /* in the order Linux numbered their buses */
static int nadded;
struct bar { struct lkl_pci_dev *dev; volatile unsigned char *va; unsigned long size; };

static int cfg(struct lkl_pci_dev *d, int where, int size, unsigned *v, int write) {
    return (int)syscall(SYS_PCI_CONFIG, d->bdf, where, size, v, write);
}
static void *irq_thread(void *p);
static struct lkl_pci_dev *pci_add(const char *name, void *ram, unsigned long size) {
    unsigned bus, dev, fn;
    if (sscanf(name, "qrt%x:%x.%x", &bus, &dev, &fn) != 3) return NULL;
    struct lkl_pci_dev *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->bdf = bus << 8 | dev << 3 | fn;
    long e = syscall(SYS_PCI_CLAIM, d->bdf);
    if (e) { fprintf(stderr, "linuxdrv: could not claim %s (%ld)\n", name + 3, e); free(d); return NULL; }
    if (nadded < MAXDEV) added[nadded++] = d;
    pthread_create(&d->th, NULL, irq_thread, d);                  /* INTx and MSI: polled */
    return d;
}
static void *irq_thread(void *p) {
    struct lkl_pci_dev *d = p;
    int idle = 0;
    while (!d->quit) {
        int hit = 0;
        if (!d->nmsi) {                                       /* INTx: the status register (a system call) */
            unsigned st = 0;
            if (cfg(d, 6, 2, &st, 0) != 2) break;             /* given back to Tessera */
            if ((st & 8) && d->irq) { lkl_trigger_irq(d->irq); hit = 1; }   /* Interrupt Status */
        }
        for (int i = 0; i < d->nmsi; i++)                     /* MSI: plain memory */
            if (d->msi[i] && __atomic_exchange_n(&d->msi[i], 0, __ATOMIC_ACQ_REL)) { lkl_trigger_irq(d->msi_irq[i]); hit = 1; }
        idle = hit ? 0 : idle + 1;
        usleep(idle > 250 ? 4000 : 1000);                     /* quiet for a while: poll less often */
    }
    return NULL;
}
static int pci_irq_init(struct lkl_pci_dev *d, int irq) {
    d->irq = irq;
    return 0;
}
static void pci_remove(struct lkl_pci_dev *d) { d->quit = 1; pthread_join(d->th, NULL); free(d); }
static int find_cap(struct lkl_pci_dev *d, unsigned id) {
    unsigned v = 0;
    if (cfg(d, 6, 2, &v, 0) != 2 || !(v & 0x10)) return 0;          /* no capability list */
    cfg(d, 0x34, 1, &v, 0);
    for (int n = 0, at = (int)(v & 0xfc); at && n < 48; n++) {
        unsigned h = 0;
        cfg(d, at, 2, &h, 0);
        if ((h & 0xff) == id) return at;
        at = (int)(h >> 8 & 0xfc);
    }
    return 0;
}
static int pci_msi_init(struct lkl_pci_dev *d, int type, int nvec, int *irqs) {
    if (nvec < 1 || nvec > MAXVEC) return -1;
    if (!d->msi) {
        long va = syscall(SYS_DMA_ALLOC, 4096, &d->msi_phys);
        if (va <= 0) return -1;
        d->msi = (volatile unsigned *)va;
    }
    d->nmsi = 0;
    for (int i = 0; i < nvec; i++) { d->msi[i] = 0; d->msi_irq[i] = irqs[i]; }
    unsigned long long a = d->msi_phys;
    if (type == LKL_PCI_IRQ_MSI) {
        int cap = find_cap(d, 0x05);
        unsigned ctl = 0, lo = (unsigned)a, hi = (unsigned)(a >> 32), one = 1;
        if (!cap || nvec != 1) return -1;                             /* one vector: one address, one word */
        cfg(d, cap + 2, 2, &ctl, 0);
        if (hi && !(ctl & 0x80)) return -1;                           /* 32-bit messages only */
        cfg(d, cap + 4, 4, &lo, 1);
        if (ctl & 0x80) { cfg(d, cap + 8, 4, &hi, 1); cfg(d, cap + 12, 2, &one, 1); }
        else cfg(d, cap + 8, 2, &one, 1);
    } else {
        int cap = find_cap(d, 0x11);
        unsigned tbl = 0;
        unsigned long long bsize = 0;
        if (!cap) return -1;
        cfg(d, cap + 4, 4, &tbl, 0);
        long va = syscall(SYS_PCI_MAPBAR, d->bdf, tbl & 7, &bsize);
        if (va <= 0 || (tbl & ~7u) + 16ull * (unsigned)nvec > bsize) return -1;
        volatile unsigned *e = (volatile unsigned *)(va + (tbl & ~7u));
        for (int i = 0; i < nvec; i++) {                             /* address, data; the mask bit stays Linux's */
            e[i * 4] = (unsigned)(a + 4ull * (unsigned)i);
            e[i * 4 + 1] = (unsigned)((a + 4ull * (unsigned)i) >> 32);
            e[i * 4 + 2] = 1;
        }
    }
    d->nmsi = nvec;
    return 0;
}
static void pci_msi_teardown(struct lkl_pci_dev *d, int type) { (void)type; d->nmsi = 0; }
static int pci_read(struct lkl_pci_dev *d, int where, int size, void *val) {
    unsigned v = 0;
    int r = cfg(d, where, size, &v, 0);
    memcpy(val, &v, (size_t)size);
    return r;
}
static int pci_write(struct lkl_pci_dev *d, int where, int size, void *val) {
    unsigned v = 0;
    memcpy(&v, val, (size_t)size);
    return cfg(d, where, size, &v, 1);
}
static int bar_rw(void *data, int off, void *buf, int size, int write) {
    struct bar *b = data;
    if (off < 0 || size <= 0 || (unsigned long)off + (unsigned long)size > b->size) return -LKL_EINVAL;
    volatile void *a = b->va + off;
    switch (size) {
    case 1: if (write) *(volatile unsigned char *)a = *(unsigned char *)buf; else *(unsigned char *)buf = *(volatile unsigned char *)a; break;
    case 2: if (write) *(volatile unsigned short *)a = *(unsigned short *)buf; else *(unsigned short *)buf = *(volatile unsigned short *)a; break;
    case 4: if (write) *(volatile unsigned *)a = *(unsigned *)buf; else *(unsigned *)buf = *(volatile unsigned *)a; break;
    case 8: if (write) *(volatile unsigned long long *)a = *(unsigned long long *)buf; else *(unsigned long long *)buf = *(volatile unsigned long long *)a; break;
    default: return -LKL_EOPNOTSUPP;
    }
    return 0;
}
static int bar_read(void *data, int off, void *res, int size) { return bar_rw(data, off, res, size, 0); }
static int bar_write(void *data, int off, void *val, int size) { return bar_rw(data, off, val, size, 1); }
static const struct lkl_iomem_ops bar_ops = { .read = bar_read, .write = bar_write };
static void *pci_resource_alloc(struct lkl_pci_dev *d, unsigned long size, int index) {
    unsigned long long bsize = 0;
    long va = syscall(SYS_PCI_MAPBAR, d->bdf, index, &bsize);
    if (va < 0 || bsize < size) return NULL;
    struct bar *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->dev = d; b->va = (volatile unsigned char *)va; b->size = size;
    return register_iomem(b, (int)size, &bar_ops);
}
static unsigned long long pci_map_page(struct lkl_pci_dev *d, void *vaddr, unsigned long size) {
    (void)d; (void)size;
    unsigned long v = (unsigned long)vaddr;
    if (v >= ram_virt && v - ram_virt < ram_size) return ram_phys + (v - ram_virt);
    long pa = syscall(SYS_DMA_ADDR, v, size);                   /* a thread's stack, static data */
    if (pa > 0) return (unsigned long long)pa;
    static int said;
    if (!said++) fprintf(stderr, "linuxdrv: DMA from memory Tessera cannot place (%p, %lu bytes)\n", vaddr, size);
    return 0;
}
static void pci_unmap_page(struct lkl_pci_dev *d, unsigned long long h, unsigned long size) { (void)d; (void)h; (void)size; }

static struct lkl_dev_pci_ops qrt_pci_ops = {
    .add = pci_add, .remove = pci_remove, .irq_init = pci_irq_init,
    .msi_init = pci_msi_init, .msi_teardown = pci_msi_teardown,
    .read = pci_read, .write = pci_write, .resource_alloc = pci_resource_alloc,
    .map_page = pci_map_page, .unmap_page = pci_unmap_page,
};

struct lkl_host_operations lkl_host_ops = {
    .print = out, .panic = panic_host,
    .sem_alloc = sem_alloc, .sem_free = sem_free, .sem_up = sem_up, .sem_down = sem_down,
    .mutex_alloc = mutex_alloc, .mutex_free = mutex_free, .mutex_lock = mutex_lock, .mutex_unlock = mutex_unlock,
    .thread_create = thread_create, .thread_detach = thread_detach, .thread_exit = thread_exit,
    .thread_join = thread_join, .thread_self = thread_self, .thread_equal = thread_equal,
    .tls_alloc = tls_alloc, .tls_free = tls_free, .tls_set = tls_set, .tls_get = tls_get,
    .mem_alloc = mem_alloc, .mem_free = free, .page_alloc = page_alloc, .page_free = page_free,
    .time = time_ns, .timer_alloc = timer_alloc, .timer_set_oneshot = timer_set_oneshot, .timer_free = timer_free,
    .ioremap = lkl_ioremap, .iomem_access = qrt_iomem_access,
    .jmp_buf_set = jmp_buf_set, .jmp_buf_longjmp = jmp_buf_longjmp,
    .memcpy = h_memcpy, .memset = h_memset, .memmove = h_memmove,
    .pci_ops = &qrt_pci_ops,
};

/* utils.c's lkl_sysctl mounts /proc through fs.c, which this host leaves out */
int lkl_mount_fs(char *fstype) { (void)fstype; return -LKL_ENOSYS; }

/* ---- the test: an HTTP request through Linux's stack ---- */
static unsigned short be16(unsigned short v);
static unsigned short be16(unsigned short v) { return (unsigned short)(v << 8 | v >> 8); }
static unsigned be32(unsigned v) { return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24; }
static int http_test(const char *hostport) {
    char host[64];
    int port = 80;
    if (sscanf(hostport, "%63[^:]:%d", host, &port) < 1) return 1;
    struct lkl_sockaddr_in sa = { .sin_family = LKL_AF_INET, .sin_port = be16((unsigned short)port) };
    unsigned a, b, c, d2;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d2) != 4) return 1;
    sa.sin_addr.lkl_s_addr = be32(a << 24 | b << 16 | c << 8 | d2);
    long s = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
    if (s < 0) { printf("linuxdrv: socket: %s\n", lkl_strerror((int)s)); return 1; }
    long r = lkl_sys_connect((int)s, (struct lkl_sockaddr *)&sa, sizeof sa);
    if (r < 0) { printf("linuxdrv: connect %s: %s\n", hostport, lkl_strerror((int)r)); return 1; }
    char req[128];
    int n = snprintf(req, sizeof req, "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
    lkl_sys_write((int)s, req, n);
    char resp[256] = { 0 };
    r = lkl_sys_read((int)s, resp, sizeof resp - 1);
    lkl_sys_close((int)s);
    if (r <= 0) { printf("linuxdrv: no answer from %s\n", hostport); return 1; }
    char *eol = strpbrk(resp, "\r\n");
    if (eol) *eol = 0;
    char line[400];
    int len = snprintf(line, sizeof line, "linuxdrv: Linux's TCP/IP and driver fetched http://%s/: %s\n", hostport, resp);
    (void)!write(1, line, (size_t)len);
    return 0;
}

/* ---- small helpers on Linux's file tree ---- */
static long rd_file(const char *path, char *buf, long cap) {          /* a sysfs file, NUL-terminated */
    long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
    if (fd < 0) return fd;
    long n = lkl_sys_read((int)fd, buf, cap - 1);
    lkl_sys_close((int)fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return n;
}
static int wr_file(const char *path, const void *data, long len) {
    long fd = lkl_sys_open(path, LKL_O_WRONLY, 0);
    if (fd < 0) return (int)fd;
    long n = lkl_sys_write((int)fd, data, len);
    lkl_sys_close((int)fd);
    return n == len ? 0 : (n < 0 ? (int)n : -LKL_EIO);
}
static int exists(const char *path) { struct lkl_stat st; return lkl_sys_stat(path, &st) == 0; }
/* the last component of a symlink's target: a driver's name from .../driver */
static int link_base(const char *path, char *out, int cap) {
    char t[256];
    long n = lkl_sys_readlink(path, t, sizeof t - 1);
    if (n <= 0) return -1;
    t[n] = 0;
    char *b = strrchr(t, '/');
    snprintf(out, (size_t)cap, "%s", b ? b + 1 : t);
    return 0;
}
/* each entry of a directory */
static int each_dirent(const char *dir, int (*fn)(const char *name, int type, void *arg), void *arg) {
    long fd = lkl_sys_open(dir, LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
    if (fd < 0) return (int)fd;
    char *buf = malloc(8192);                                       /* fn may walk another directory */
    long n;
    int stop = 0;
    while (!stop && (n = lkl_sys_getdents64((unsigned)fd, (struct lkl_linux_dirent64 *)buf, 8192)) > 0)
        for (long o = 0; o < n && !stop; ) {
            struct lkl_linux_dirent64 *d = (struct lkl_linux_dirent64 *)(buf + o);
            o += d->d_reclen;
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
            stop = fn(d->d_name, d->d_type, arg);
        }
    free(buf);
    lkl_sys_close((int)fd);
    return stop;
}

/* ---- firmware: request_firmware() answered from Tessera's /lib/firmware ---- */
static int fw_one(const char *name, int type, void *arg) {
    (void)type; (void)arg;
    if (!strcmp(name, "timeout")) return 0;
    char file[300], dir[300], p[340];
    snprintf(file, sizeof file, "/lib/firmware/%s", name);
    for (char *c = file; *c; c++) if (*c == '!') *c = '/';          /* the loader writes '/' as '!' */
    snprintf(dir, sizeof dir, "/sys/class/firmware/%s", name);
    snprintf(p, sizeof p, "%s/loading", dir);
    int fd = open(file, O_RDONLY);                                  /* Tessera's file, through libc */
    if (fd < 0) {
        printf("linuxdrv: firmware %s: not in /lib/firmware\n", file + 14);
        wr_file(p, "-1", 2);
        return 0;
    }
    wr_file(p, "1", 1);
    char d[340];
    snprintf(d, sizeof d, "%s/data", dir);
    long lfd = lkl_sys_open(d, LKL_O_WRONLY, 0);
    static char buf[65536];
    long total = 0, n;
    while (lfd >= 0 && (n = read(fd, buf, sizeof buf)) > 0) {
        if (lkl_sys_write((int)lfd, buf, n) != n) { total = -1; break; }
        total += n;
    }
    close(fd);
    if (lfd >= 0) lkl_sys_close((int)lfd);
    wr_file(p, total > 0 ? "0" : "-1", total > 0 ? 1 : 2);
    printf("linuxdrv: firmware %s (%ld bytes) loaded\n", file + 14, total);
    return 0;
}
static void *fw_thread(void *a) {
    (void)a;
    for (;;) { each_dirent("/sys/class/firmware", fw_one, NULL); usleep(50000); }
    return NULL;
}

/* ---- which driver Linux bound to each device: Tessera shows it, or gets the device back ---- */
#define SYS_PCI_REPORT 1048
static void report_devices(void) {
    int left = nadded, reported[MAXDEV] = { 0 };
    for (int t = 0; t < 50 && left; t++) {                          /* asynchronous probes: up to 5 s */
        for (int i = 0; i < nadded; i++) {
            if (reported[i]) continue;
            char p[96], drv[64];
            snprintf(p, sizeof p, "/sys/bus/pci/devices/0000:%02x:00.0/driver", i);
            if (link_base(p, drv, sizeof drv)) continue;
            syscall(SYS_PCI_REPORT, added[i]->bdf, drv);
            printf("linuxdrv: %02x:%02x.%x: Linux's %s\n", added[i]->bdf >> 8, added[i]->bdf >> 3 & 31, added[i]->bdf & 7, drv);
            reported[i] = 1; left--;
        }
        if (left) usleep(100000);
    }
    for (int i = 0; i < nadded; i++)
        if (!reported[i]) {
            syscall(SYS_PCI_REPORT, added[i]->bdf, NULL);
            printf("linuxdrv: %02x:%02x.%x: no Linux driver either; back to Tessera\n", added[i]->bdf >> 8, added[i]->bdf >> 3 & 31, added[i]->bdf & 7);
        }
}

/* ---- network cards: every Ethernet interface's frames <-> an interface in Tessera ---- */
#define SYS_NET_ATTACH 1045
#define SYS_NET_RX     1046
#define SYS_NET_TX     1047
struct sll { unsigned short family, protocol; int ifindex; unsigned short hatype; unsigned char pkttype, halen, addr[8]; };
struct nbridge { int pkt; long handle; char name[16]; };
#define MAXNET 4
static struct nbridge nets[MAXNET];
static int nnets;

static void *bridge_rx(void *arg) {
    struct nbridge *b = arg;
    unsigned char *f = malloc(2048);
    for (;;) {
        long n = lkl_sys_read(b->pkt, (char *)f, 2048);
        if (n > 0 && n <= 1600) syscall(SYS_NET_RX, b->handle, f, n);
        else if (n < 0 && n != -LKL_EINTR) { usleep(100000); }
    }
    return NULL;
}
static void *bridge_tx(void *arg) {
    struct nbridge *b = arg;
    unsigned char *f = malloc(2048);
    for (;;) {
        long n = syscall(SYS_NET_TX, b->handle, f, 2048, 200);
        if (n > 0) lkl_sys_write(b->pkt, (char *)f, n);
    }
    return NULL;
}
static int bridge(const char *ifname) {
    long s = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct lkl_ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.lkl_ifr_name, ifname, LKL_IFNAMSIZ - 1);
    if (lkl_sys_ioctl((int)s, LKL_SIOCGIFINDEX, (long)&ifr) < 0) { lkl_sys_close((int)s); return -1; }
    int ifindex = ifr.lkl_ifr_ifindex;
    unsigned char mac[6];
    lkl_sys_ioctl((int)s, LKL_SIOCGIFHWADDR, (long)&ifr);
    memcpy(mac, ifr.lkl_ifr_hwaddr.sa_data, 6);
    struct { unsigned cmd, data; } ev = { 0x2c, 0 };                /* ETHTOOL_SGRO: off - one frame in, one frame out */
    ifr.lkl_ifr_data = &ev;
    lkl_sys_ioctl((int)s, LKL_SIOCETHTOOL, (long)&ifr);
    lkl_sys_ioctl((int)s, LKL_SIOCGIFFLAGS, (long)&ifr);
    ifr.lkl_ifr_flags |= LKL_IFF_UP | LKL_IFF_PROMISC;
    long up = lkl_sys_ioctl((int)s, LKL_SIOCSIFFLAGS, (long)&ifr);
    lkl_sys_close((int)s);
    if (up < 0) { printf("linuxdrv: %s did not come up: %s\n", ifname, lkl_strerror((int)up)); return -1; }
    struct nbridge *b = &nets[nnets];
    b->pkt = (int)lkl_sys_socket(LKL_AF_PACKET, LKL_SOCK_RAW, be16(0x0003));   /* ETH_P_ALL */
    if (b->pkt < 0) return -1;
    struct sll a = { .family = LKL_AF_PACKET, .protocol = be16(0x0003), .ifindex = ifindex };
    if (lkl_sys_bind(b->pkt, (struct lkl_sockaddr *)&a, sizeof a) < 0) { lkl_sys_close(b->pkt); return -1; }
    char p[96], drv[32] = "";
    snprintf(p, sizeof p, "/sys/class/net/%s/device/driver", ifname);
    link_base(p, drv, sizeof drv);
    b->handle = syscall(SYS_NET_ATTACH, mac, drv[0] ? drv : NULL);
    if (b->handle < 0) { printf("linuxdrv: Tessera refused %s (%d)\n", ifname, errno); lkl_sys_close(b->pkt); return -1; }
    snprintf(b->name, sizeof b->name, "%s", ifname);
    nnets++;
    pthread_t t1, t2;
    pthread_create(&t1, NULL, bridge_rx, b);
    pthread_create(&t2, NULL, bridge_tx, b);
    printf("linuxdrv: %s (%s, %02x:%02x:%02x:%02x:%02x:%02x) bridged into Tessera's network stack\n", ifname, drv,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
static int net_one(const char *name, int type, void *arg) {
    (void)type; (void)arg;
    if (!strcmp(name, "lo") || nnets >= MAXNET) return 0;
    for (int i = 0; i < nnets; i++) if (!strcmp(nets[i].name, name)) return 0;
    char p[96], t[16];
    snprintf(p, sizeof p, "/sys/class/net/%s/type", name);
    if (rd_file(p, t, sizeof t) <= 0 || strcmp(t, "1")) return 0;          /* ARPHRD_ETHER only */
    snprintf(p, sizeof p, "/sys/class/net/%s/wireless", name);
    if (exists(p)) return 0;                                               /* Wi-Fi needs a supplicant: not yet */
    snprintf(p, sizeof p, "/sys/class/net/%s/device", name);
    if (!exists(p)) return 0;                                              /* virtual interfaces */
    bridge(name);
    return 0;
}

/* ---- disks: each file system Linux can mount -> /mnt/<name> in Tessera ---- */
#define SYS_FS_ATTACH 1049
#define SYS_FS_SERVE  1050
#define SYS_FS_REPLY  1051
#define SYS_FS_DETACH 1052
enum { VR_LIST, VR_READ, VR_WRITE, VR_CREATE, VR_MKDIR, VR_UNLINK, VR_RMDIR, VR_RENAME, VR_TRUNC };
typedef struct { unsigned id, mnt, op, pad; unsigned long long off, len; char path[512], path2[512]; } fsreq_t;
#define MAXMNT 16
static struct dmount { char name[32]; long handle; int used, tried; } mounts[MAXMNT];
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;

static struct dmount *mount_named(const char *name) {
    for (int i = 0; i < MAXMNT; i++) if ((mounts[i].used || mounts[i].tried) && !strcmp(mounts[i].name, name)) return &mounts[i];
    return NULL;
}
static int try_mount(const char *name) {
    static const char *const types[] = { "ext4", "vfat", "exfat", "ntfs3", "xfs", "btrfs", "f2fs", "iso9660", "udf", NULL };
    char dev[64], dir[64];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    snprintf(dir, sizeof dir, "/mnt/%s", name);
    lkl_sys_mkdir(dir, 0755);
    for (int i = 0; types[i]; i++) {
        int ro = !strcmp(types[i], "ntfs3") || !strcmp(types[i], "iso9660") || !strcmp(types[i], "udf");
        long r = lkl_sys_mount(dev, dir, (char *)types[i], ro ? LKL_MS_RDONLY : 0, NULL);
        if (r == -LKL_EROFS || r == -LKL_EACCES) { r = lkl_sys_mount(dev, dir, (char *)types[i], LKL_MS_RDONLY, NULL); ro = 1; }
        if (r == 0) return ro ? -(i + 1) - 100 : i + 1;
    }
    lkl_sys_rmdir(dir);
    return 0;
}
static int disk_of(const char *name, char *disk, int cap) {        /* sda1 -> sda, nvme0n1p2 -> nvme0n1 */
    char p[96], t[256];
    snprintf(p, sizeof p, "/sys/class/block/%s", name);
    long n = lkl_sys_readlink(p, t, sizeof t - 1);
    if (n <= 0) return -1;
    t[n] = 0;
    char *e = strrchr(t, '/');
    if (!e) return -1;
    *e = 0;
    char *b = strrchr(t, '/');
    snprintf(disk, (size_t)cap, "%s", b ? b + 1 : t);
    return 0;
}
static int has_parts(const char *name, int type, void *arg) {     /* another block device is a partition of arg */
    (void)type;
    const char *disk = arg;
    if (!strcmp(name, disk)) return 0;
    char d[64];
    return !disk_of(name, d, sizeof d) && !strcmp(d, disk);
}
static int block_one(const char *name, int type, void *arg) {
    (void)type; (void)arg;
    if (!strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "zram", 4) || strstr(name, "boot") || strstr(name, "rpmb")) return 0;
    pthread_mutex_lock(&mlock);
    int known = mount_named(name) != NULL;
    pthread_mutex_unlock(&mlock);
    if (known) return 0;
    char p[96], v[64];
    snprintf(p, sizeof p, "/sys/class/block/%s/size", name);
    if (rd_file(p, v, sizeof v) <= 0 || !strcmp(v, "0")) return 0;
    long long sectors = atoll(v);
    snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    int part = exists(p);
    if (!part && each_dirent("/sys/class/block", has_parts, (void *)name) == 1) return 0;   /* use its partitions */
    struct dmount *m = NULL;
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < MAXMNT && !m; i++) if (!mounts[i].used && !mounts[i].tried) m = &mounts[i];
    if (m) { memset(m, 0, sizeof *m); snprintf(m->name, sizeof m->name, "%s", name); m->tried = 1; }
    pthread_mutex_unlock(&mlock);
    if (!m) return 0;
    int r = try_mount(name);
    if (!r) { printf("linuxdrv: %s: no file system Linux can mount\n", name); return 0; }
    static const char *const names[] = { "ext4", "FAT", "exFAT", "NTFS", "XFS", "Btrfs", "F2FS", "ISO 9660", "UDF" };
    int ro = r < 0, ti = (ro ? -r - 100 : r) - 1;
    char disk[64] = "", model[64] = "", what[64];
    if (part) disk_of(name, disk, sizeof disk); else snprintf(disk, sizeof disk, "%s", name);
    snprintf(p, sizeof p, "/sys/class/block/%s/device/model", disk);
    rd_file(p, model, sizeof model);
    double gb = (double)sectors * 512 / 1e9;
    char sz[24];
    if (gb < 1) snprintf(sz, sizeof sz, "%.0f MB", gb * 1000); else snprintf(sz, sizeof sz, "%.*f GB", gb < 10 ? 1 : 0, gb);
    snprintf(what, sizeof what, "%s%s, %s%s%s", names[ti], ro ? " read-only" : "", sz, model[0] ? " on " : "", model);
    long h = syscall(SYS_FS_ATTACH, name, what);
    if (h < 0) { printf("linuxdrv: Tessera refused /mnt/%s (%d)\n", name, errno); return 0; }
    pthread_mutex_lock(&mlock);
    m->handle = h; m->used = 1;
    pthread_mutex_unlock(&mlock);
    printf("linuxdrv: %s (%s) is /mnt/%s\n", name, what, name);
    return 0;
}
static void blocks_gone(void) {                                    /* unplugged: unmount, leave /mnt */
    for (int i = 0; i < MAXMNT; i++) {
        struct dmount *m = &mounts[i];
        if (!m->used && !m->tried) continue;
        char p[96];
        snprintf(p, sizeof p, "/sys/class/block/%s", m->name);
        if (exists(p)) continue;
        if (m->used) {
            snprintf(p, sizeof p, "/mnt/%s", m->name);
            lkl_sys_umount(p, 2 /* MNT_DETACH */);
            syscall(SYS_FS_DETACH, m->handle);
            printf("linuxdrv: %s went away\n", m->name);
        }
        pthread_mutex_lock(&mlock);
        m->used = m->tried = 0;
        pthread_mutex_unlock(&mlock);
    }
}

/* LIST: { u8 dir; u16 mode; u64 size; u8 len; name } per entry */
static long fs_list(const char *dir, char *out, unsigned long long cap) {
    long fd = lkl_sys_open(dir, LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
    if (fd < 0) return fd;
    static __thread char buf[16384];
    long n, at = 0;
    while ((n = lkl_sys_getdents64((unsigned)fd, (struct lkl_linux_dirent64 *)buf, sizeof buf)) > 0)
        for (long o = 0; o < n; ) {
            struct lkl_linux_dirent64 *d = (struct lkl_linux_dirent64 *)(buf + o);
            o += d->d_reclen;
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
            size_t len = strlen(d->d_name);
            if (len > 63 || (unsigned long long)at + 12 + len > cap) continue;   /* Tessera's names stop at 63 */
            struct lkl_stat st;
            if (lkl_sys_newfstatat((int)fd, d->d_name, &st, 0) < 0) continue;
            int isdir = (st.st_mode & 0170000) == 0040000;
            if (!isdir && (st.st_mode & 0170000) != 0100000) continue;           /* files and directories */
            unsigned long long size = (unsigned long long)st.st_size;
            out[at] = (char)isdir;
            out[at + 1] = (char)(st.st_mode & 0xff); out[at + 2] = (char)((st.st_mode >> 8) & 0x0f);
            memcpy(out + at + 3, &size, 8);
            out[at + 11] = (char)len;
            memcpy(out + at + 12, d->d_name, len);
            at += 12 + (long)len;
        }
    lkl_sys_close((int)fd);
    return at;
}
/* writes reach the disk a second after the last change, and all of them before Tessera powers off */
static volatile int dirty;
static volatile unsigned long long dirty_at;
static void *flusher(void *arg) {
    (void)arg;
    for (;;) {
        usleep(200000);
        if (dirty && time_ns() - dirty_at > 1000000000ull) { dirty = 0; lkl_sys_sync(); }
    }
    return NULL;
}
static volatile sig_atomic_t stopping;
static void on_term(int sig) { (void)sig; stopping = 1; }
static void *stopper(void *arg) {                                  /* SIGTERM: sync, unmount, go */
    (void)arg;
    while (!stopping) usleep(50000);
    lkl_sys_sync();
    for (int i = 0; i < MAXMNT; i++)
        if (mounts[i].used) {
            char p[64];
            snprintf(p, sizeof p, "/mnt/%s", mounts[i].name);
            lkl_sys_umount(p, 0);
        }
    printf("linuxdrv: disks synced and unmounted\n");
    _exit(0);
    return NULL;
}

static void *fs_server(void *arg) {
    (void)arg;
    fsreq_t q;
    size_t cap = 256 << 10;
    char *buf = malloc(cap);
    for (;;) {
        long got = syscall(SYS_FS_SERVE, &q, buf, cap, 1000);
        if (got <= 0) { if (got < 0) usleep(100000); continue; }
        char path[600], path2[600];
        long r = -LKL_EIO, out = 0;
        pthread_mutex_lock(&mlock);
        const char *mname = NULL;
        for (int i = 0; i < MAXMNT; i++) if (mounts[i].used && mounts[i].handle == (long)q.mnt) mname = mounts[i].name;
        if (mname) {
            snprintf(path, sizeof path, "/mnt/%s%s", mname, strcmp(q.path, "/") ? q.path : "");
            snprintf(path2, sizeof path2, "/mnt/%s%s", mname, q.path2);
        }
        pthread_mutex_unlock(&mlock);
        if (!mname) { syscall(SYS_FS_REPLY, q.id, -LKL_ENODEV, NULL, 0); continue; }
        unsigned long long len = q.len > cap ? cap : q.len;
        long fd;
        switch (q.op) {
        case VR_LIST: r = fs_list(path, buf, len); out = r; break;
        case VR_READ:
            fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
            r = fd < 0 ? fd : lkl_sys_pread64((unsigned)fd, buf, len, (lkl_loff_t)q.off);
            if (fd >= 0) lkl_sys_close((int)fd);
            out = r;
            break;
        case VR_WRITE:
            fd = lkl_sys_open(path, LKL_O_WRONLY, 0);
            r = fd < 0 ? fd : lkl_sys_pwrite64((unsigned)fd, buf, len, (lkl_loff_t)q.off);
            if (fd >= 0) lkl_sys_close((int)fd);
            break;
        case VR_CREATE:
            fd = lkl_sys_open(path, LKL_O_WRONLY | LKL_O_CREAT, 0644);
            r = fd < 0 ? fd : 0;
            if (fd >= 0) lkl_sys_close((int)fd);
            break;
        case VR_MKDIR:  r = lkl_sys_mkdir(path, 0755); break;
        case VR_UNLINK: r = lkl_sys_unlink(path); break;
        case VR_RMDIR:  r = lkl_sys_rmdir(path); break;
        case VR_RENAME: r = lkl_sys_rename(path, path2); break;
        case VR_TRUNC:  r = lkl_sys_truncate(path, 0); break;
        }
        if (q.op != VR_LIST && q.op != VR_READ && r >= 0) { dirty_at = time_ns(); dirty = 1; }
        syscall(SYS_FS_REPLY, q.id, r, out > 0 ? buf : NULL, out > 0 ? out : 0);
    }
    return NULL;
}

/* ---- keyboards, mice, touch screens (evdev) -> the shell's events ---- */
#define SYS_INPUT 1053
/* Linux key codes -> HID keyboard usages (the inverse of Linux's hid_keyboard[]) */
static const unsigned char hid_keyboard[0x68] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117 };
static unsigned char usage_of[256];
struct ievent { long sec, usec; unsigned short type, code; int value; };
struct indev { int fd, abs; int min[2], max[2]; char name[16]; };
#define MAXIN 8
static struct indev indevs[MAXIN];
static int nin;

#define EVIOCGBIT(ev, len) ((2u << 30) | ((unsigned)(len) << 16) | ('E' << 8) | (0x20 + (ev)))
#define EVIOCGABS(abs)     ((2u << 30) | (24u << 16) | ('E' << 8) | (0x40 + (abs)))
static int bit(const unsigned char *b, int n) { return b[n / 8] >> (n % 8) & 1; }

static void *input_thread(void *arg) {
    struct indev *d = arg;
    struct ievent ev[16];
    int shift = 0, ctrl = 0, dx = 0, dy = 0, wheel = 0, ax = 0, ay = 0, moved = 0;
    unsigned buttons = 0, sent_buttons = 0;
    for (;;) {
        long n = lkl_sys_read(d->fd, (char *)ev, sizeof ev);
        if (n <= 0) { if (n == -LKL_ENODEV) break; usleep(10000); continue; }
        for (long i = 0; i < n / (long)sizeof ev[0]; i++) {
            struct ievent *e = &ev[i];
            if (e->type == 1) {                                     /* EV_KEY */
                int c = e->code;
                if (c == 42 || c == 54) { shift = e->value != 0; continue; }
                if (c == 29 || c == 97) { ctrl = e->value != 0; continue; }
                if (c == 0x110 || c == 0x14a) buttons = e->value ? buttons | 1 : buttons & ~1u;   /* left, touch */
                else if (c == 0x111) buttons = e->value ? buttons | 2 : buttons & ~2u;
                else if (c == 0x112) buttons = e->value ? buttons | 4 : buttons & ~4u;
                else if (c < 256 && usage_of[c] && e->value) syscall(SYS_INPUT, 0, usage_of[c], shift, ctrl, 0);   /* press, repeat */
            } else if (e->type == 2 && !d->abs) {                   /* EV_REL */
                if (e->code == 0) dx += e->value; else if (e->code == 1) dy += e->value; else if (e->code == 8) wheel += e->value;
                moved = 1;
            } else if (e->type == 2 && e->code == 8) {             /* a tablet's wheel */
                wheel += e->value;
                moved = 1;
            } else if (e->type == 3 && d->abs) {                   /* EV_ABS: X, Y or the first contact */
                int axis = e->code == 0 || e->code == 0x35 ? 0 : e->code == 1 || e->code == 0x36 ? 1 : -1;
                if (axis < 0) continue;
                int span = d->max[axis] - d->min[axis];
                int v = span > 0 ? (int)((long long)(e->value - d->min[axis]) * 65535 / span) : 0;
                if (axis == 0) ax = v; else ay = v;
                moved = 1;
            } else if (e->type == 0 && e->code == 0) {              /* SYN_REPORT */
                if (moved || buttons != sent_buttons) {
                    if (d->abs) syscall(SYS_INPUT, 2, (unsigned long)(unsigned)ax | (unsigned long)(unsigned)ay << 32, (unsigned long)(unsigned)(-wheel), buttons, 0);
                    else syscall(SYS_INPUT, 1, (unsigned long)(unsigned)dx | (unsigned long)(unsigned)dy << 32, (unsigned long)(unsigned)(-wheel), buttons, 0);
                }
                dx = dy = wheel = moved = 0;
                sent_buttons = buttons;
            }
        }
    }
    printf("linuxdrv: %s went away\n", d->name);
    lkl_sys_close(d->fd);
    d->fd = -1;
    return NULL;
}
static int input_one(const char *name, int type, void *arg) {
    (void)type; (void)arg;
    if (strncmp(name, "event", 5) || nin >= MAXIN) return 0;
    for (int i = 0; i < nin; i++) if (indevs[i].fd >= 0 && !strcmp(indevs[i].name, name)) return 0;
    char p[64];
    snprintf(p, sizeof p, "/dev/input/%s", name);
    long fd = lkl_sys_open(p, LKL_O_RDONLY, 0);
    if (fd < 0) return 0;
    unsigned char evb[4] = { 0 }, keys[96] = { 0 }, relb[2] = { 0 }, absb[8] = { 0 };
    lkl_sys_ioctl((int)fd, EVIOCGBIT(0, sizeof evb), (long)evb);
    lkl_sys_ioctl((int)fd, EVIOCGBIT(1, sizeof keys), (long)keys);
    lkl_sys_ioctl((int)fd, EVIOCGBIT(2, sizeof relb), (long)relb);
    lkl_sys_ioctl((int)fd, EVIOCGBIT(3, sizeof absb), (long)absb);
    /* a tablet may have a wheel too: relative means REL_X */
    int kbd = bit(evb, 1) && bit(keys, 30), rel = bit(evb, 2) && bit(relb, 0), abs = bit(evb, 3) && bit(absb, 0) && bit(absb, 1);
    if (!kbd && !rel && !abs) { lkl_sys_close((int)fd); return 0; }
    struct indev *d = NULL;
    for (int i = 0; i < nin && !d; i++) if (indevs[i].fd < 0) d = &indevs[i];
    if (!d) d = &indevs[nin++];
    memset(d, 0, sizeof *d);
    d->fd = (int)fd;
    d->abs = abs && !rel;
    snprintf(d->name, sizeof d->name, "%s", name);
    for (int a = 0; a < 2 && d->abs; a++) {
        int info[6] = { 0 };
        lkl_sys_ioctl((int)fd, EVIOCGABS(a), (long)info);
        d->min[a] = info[1]; d->max[a] = info[2];
    }
    char nm[96] = "", sp[96];
    snprintf(sp, sizeof sp, "/sys/class/input/%s/device/name", name);
    rd_file(sp, nm, sizeof nm);
    printf("linuxdrv: %s: %s (%s) into Tessera's input\n", name, nm, kbd ? "keyboard" : d->abs ? "touch / absolute pointer" : "mouse");
    pthread_t t;
    pthread_create(&t, NULL, input_thread, d);
    return 0;
}

/* ---- sound: the first ALSA playback device plays Tessera's mix ---- */
#define SYS_SND_ATTACH 1054
#define SYS_SND_PULL   1055
struct snd_interval { unsigned min, max, flags; };
struct snd_hw_params {
    unsigned flags, masks[3][8], mres[5][8];
    struct snd_interval intervals[12], ires[9];
    unsigned rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};
_Static_assert(sizeof(struct snd_hw_params) == 608, "snd_pcm_hw_params");
struct snd_xferi { long result; void *buf; unsigned long frames; };
struct snd_ctl_id { unsigned numid; int iface; unsigned device, subdevice; unsigned char name[44]; unsigned index; };
struct snd_ctl_list { unsigned offset, space, used, count; struct snd_ctl_id *pids; unsigned char reserved[50]; };
struct snd_ctl_info { struct snd_ctl_id id; int type; unsigned access, count; int owner; union { struct { long min, max, step; } integer; unsigned char r[128]; } value; unsigned char reserved[64]; };
struct snd_ctl_value { struct snd_ctl_id id; unsigned indirect; union { long integer[128]; unsigned char r[1024]; } value; unsigned char reserved[128]; };
_Static_assert(sizeof(struct snd_ctl_info) == 272, "snd_ctl_elem_info");
_Static_assert(sizeof(struct snd_ctl_value) == 1224, "snd_ctl_elem_value");
#define IOWR(t, nr, sz) ((3u << 30) | ((unsigned)(sz) << 16) | ((t) << 8) | (nr))
#define IOW(t, nr, sz)  ((1u << 30) | ((unsigned)(sz) << 16) | ((t) << 8) | (nr))

static void mixer_up(int card) {                                   /* unmute playback, volumes to 80% */
    char p[48];
    snprintf(p, sizeof p, "/dev/snd/controlC%d", card);
    long fd = lkl_sys_open(p, LKL_O_RDWR, 0);
    if (fd < 0) return;
    static struct snd_ctl_id ids[256];
    struct snd_ctl_list l = { .space = 256, .pids = ids };
    if (lkl_sys_ioctl((int)fd, IOWR('U', 0x10, sizeof l), (long)&l) == 0)
        for (unsigned i = 0; i < l.used; i++) {
            const char *nm = (const char *)ids[i].name;
            int sw = strstr(nm, "Playback Switch") != NULL, vol = strstr(nm, "Playback Volume") != NULL;
            if (!sw && !vol) continue;
            struct snd_ctl_info inf = { .id = ids[i] };
            if (lkl_sys_ioctl((int)fd, IOWR('U', 0x11, sizeof inf), (long)&inf)) continue;
            struct snd_ctl_value v = { .id = ids[i] };
            for (unsigned c = 0; c < inf.count && c < 128; c++)
                v.value.integer[c] = sw ? 1 : inf.value.integer.min + (inf.value.integer.max - inf.value.integer.min) * 4 / 5;
            lkl_sys_ioctl((int)fd, IOWR('U', 0x13, sizeof v), (long)&v);
        }
    lkl_sys_close((int)fd);
}
static void *sound_thread(void *arg) {
    char *dev = arg;
    long fd = lkl_sys_open(dev, LKL_O_RDWR, 0);
    if (fd < 0) { printf("linuxdrv: %s: %s\n", dev, lkl_strerror((int)fd)); return NULL; }
    struct snd_hw_params hp;
    unsigned rate = 0;
    static const unsigned rates[] = { 48000, 44100 };
    for (int r = 0; r < 2 && !rate; r++) {
        memset(&hp, 0, sizeof hp);
        for (int m = 0; m < 3; m++) memset(hp.masks[m], 0, sizeof hp.masks[m]);
        hp.masks[0][0] = 1u << 3;                                   /* RW_INTERLEAVED */
        hp.masks[1][0] = 1u << 2;                                   /* S16_LE */
        hp.masks[2][0] = 1u << 0;                                   /* STD */
        for (int i = 0; i < 12; i++) { hp.intervals[i].min = 0; hp.intervals[i].max = ~0u; }
        hp.intervals[10 - 8].min = hp.intervals[10 - 8].max = 2;    /* CHANNELS */
        hp.intervals[11 - 8].min = hp.intervals[11 - 8].max = rates[r];
        hp.intervals[16 - 8].min = 40000; hp.intervals[16 - 8].max = 100000;   /* BUFFER_TIME, us */
        hp.rmask = ~0u;
        if (lkl_sys_ioctl((int)fd, IOWR('A', 0x11, sizeof hp), (long)&hp) == 0) rate = rates[r];
    }
    if (!rate) { printf("linuxdrv: %s: no 16-bit stereo at 48 or 44.1 kHz\n", dev); lkl_sys_close((int)fd); return NULL; }
    lkl_sys_ioctl((int)fd, 0x4140, 0);                              /* PREPARE */
    char name[64], cp[64], id[48] = "";
    int card = dev[strlen("/dev/snd/pcmC")] - '0';
    snprintf(cp, sizeof cp, "/sys/class/sound/card%d/id", card);
    rd_file(cp, id, sizeof id);
    mixer_up(card);
    snprintf(name, sizeof name, "Linux: %s", id[0] ? id : "sound card");
    long h = syscall(SYS_SND_ATTACH, name, rate, 2);
    if (h < 0) { printf("linuxdrv: Tessera refused the sound output (%d)\n", errno); lkl_sys_close((int)fd); return NULL; }
    printf("linuxdrv: %s plays Tessera's sound (%u Hz)\n", dev, rate);
    short *buf = malloc(1024 * 4);
    for (;;) {
        long n = syscall(SYS_SND_PULL, h, buf, 1024, 100);
        if (n <= 0) continue;
        struct snd_xferi x = { 0, buf, (unsigned long)n };
        long r = lkl_sys_ioctl((int)fd, IOW('A', 0x50, sizeof x), (long)&x);
        if (r == -LKL_EPIPE || r == -LKL_ESTRPIPE) { lkl_sys_ioctl((int)fd, 0x4140, 0); lkl_sys_ioctl((int)fd, IOW('A', 0x50, sizeof x), (long)&x); }
    }
    return NULL;
}
static int sound_started;
static int pcm_one(const char *name, int type, void *arg) {
    (void)type;
    char *best = arg;
    if (strncmp(name, "pcmC", 4) || name[strlen(name) - 1] != 'p') return 0;
    if (!best[0] || strcmp(name, best + 9) < 0) snprintf(best, 64, "/dev/snd/%s", name);   /* lowest card and device */
    return 0;
}

/* ---- hot-plug: interfaces, disks, input devices, sound cards Linux finds later ---- */
static void *hotplug_thread(void *arg) {
    (void)arg;
    for (;;) {
        each_dirent("/sys/class/net", net_one, NULL);
        blocks_gone();
        each_dirent("/sys/class/block", block_one, NULL);
        each_dirent("/dev/input", input_one, NULL);
        if (!sound_started) {
            static char best[64];
            best[0] = 0;
            each_dirent("/dev/snd", pcm_one, best);
            if (best[0]) { sound_started = 1; pthread_t t; pthread_create(&t, NULL, sound_thread, best); }
        }
        sleep(2);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *test = NULL;
    char devs[512] = "", extra[512] = "";
    int ndev = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test") && i + 1 < argc) { test = argv[++i]; continue; }
        if (!strcmp(argv[i], "--")) {
            for (i++; i < argc; i++) { strncat(extra, " ", sizeof extra - strlen(extra) - 1); strncat(extra, argv[i], sizeof extra - strlen(extra) - 1); }
            break;
        }
        snprintf(devs + strlen(devs), sizeof devs - strlen(devs), "%sqrt%s", ndev++ ? "," : "", argv[i]);
    }
    if (!ndev) {
        fprintf(stderr, "usage: linuxdrv [--test host:port] <bus:dev.fn>... [-- Linux kernel arguments]\n");
        return 2;
    }
    FILE *cf = fopen("/etc/linuxdrv.conf", "r");                   /* "linux <arguments>": more kernel arguments */
    for (char line[256]; cf && fgets(line, sizeof line, cf); )
        if (!strncmp(line, "linux ", 6)) {
            line[strcspn(line, "\n")] = 0;
            strncat(extra, " ", sizeof extra - strlen(extra) - 1);
            strncat(extra, line + 6, sizeof extra - strlen(extra) - 1);
        }
    if (cf) fclose(cf);
    for (int i = 0; i < 0x68; i++) if (hid_keyboard[i]) usage_of[hid_keyboard[i]] = (unsigned char)i;
    usage_of[43] = 0x31;                                            /* backslash: not the non-US # */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (lkl_init(&lkl_host_ops) < 0) { fprintf(stderr, "linuxdrv: lkl_init failed\n"); return 1; }
    char cmd[1200];
    /* --test: the card at boot, for ip=dhcp; otherwise the devices come once the firmware helper runs */
    snprintf(cmd, sizeof cmd, "mem=96M loglevel=4 %s%s %s%s", test ? "lkl_pci=" : "", test ? devs : "",
             test ? "ip=dhcp" : "ipv6.disable=1", extra);
    printf("linuxdrv: starting Linux (%s)\n", cmd);
    long r = lkl_start_kernel(cmd);
    if (r < 0) { fprintf(stderr, "linuxdrv: Linux did not start: %s\n", lkl_strerror((int)r)); return 1; }
    printf("linuxdrv: Linux is running\n");
    if (test) {
        int ok = 1;
        for (int i = 0; i < 20 && ok; i++) { ok = http_test(test); if (ok) sleep(1); }
        printf("linuxdrv: test %s\n", ok ? "FAILED" : "passed");
        lkl_sys_halt();
        return ok;
    }
    lkl_sys_mkdir("/sys", 0755);
    lkl_sys_mkdir("/dev", 0755);
    lkl_sys_mkdir("/mnt", 0755);
    lkl_sys_mount("sysfs", "/sys", "sysfs", 0, NULL);
    lkl_sys_mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    wr_file("/sys/class/firmware/timeout", "30", 2);
    pthread_t t;
    pthread_create(&t, NULL, fw_thread, NULL);
    long e = wr_file("/sys/module/lkl_pci/parameters/devices", devs, (long)strlen(devs));
    if (e) printf("linuxdrv: adding the devices: %s\n", lkl_strerror((int)e));
    report_devices();
    for (int i = 0; i < 4; i++) pthread_create(&t, NULL, fs_server, NULL);
    pthread_create(&t, NULL, flusher, NULL);
    pthread_create(&t, NULL, stopper, NULL);
    signal(SIGTERM, on_term);
    pthread_create(&t, NULL, hotplug_thread, NULL);
    for (;;) pause();                                               /* the driver host stays up */
}
