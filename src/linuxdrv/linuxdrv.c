/*
 * linuxdrv - Linux's device drivers on QRT: the Linux kernel as a library (LKL, the
 * anykernel / LibOS approach) in a user program, with Tessera as its host.
 *
 *   linuxdrv <bus:dev.fn> [--test host:port] [Linux kernel arguments]
 *
 * Tessera keeps its own personality - the shell, the native and Linux system calls, the
 * scheduler - and a driver host runs one PCI device with the unmodified Linux driver for
 * it, inside a complete Linux kernel (scheduler, memory manager, network stack, sysfs)
 * that runs as threads of this process.  This file is LKL's "host": the operations
 * LKL asks of an operating system (lkl_host.h), implemented on Tessera:
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
 * A network card is bridged into Tessera: Linux drives the card and nothing more (no
 * address on eth0, IPv6 off, GRO off so frames stay frames); a packet socket on eth0
 * passes every Ethernet frame to a network interface in Tessera's own stack (system
 * calls 1045-1047), which does ARP, DHCP and TCP for QRT's programs as it does for the
 * cards it drives itself.
 *
 * --test host:port instead gives eth0 to Linux's own TCP/IP (DHCP at boot), fetches
 * http://host:port/ through it and prints the status line (QEMU: an e1000e and the
 * test web server).
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
#include <lkl.h>
#include <lkl_host.h>
#include "lkl/iomem.h"

/* ---- Tessera's driver-host system calls (src/arch/x64/lkldev.c) ---- */
#define SYS_PCI_CLAIM  1040
#define SYS_PCI_CONFIG 1041
#define SYS_PCI_MAPBAR 1042
#define SYS_DMA_ALLOC  1043

static unsigned long ram_virt;
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
    ram_phys = phys;
    return (void *)va;
}
static void page_free(void *addr, unsigned long size) { (void)addr; (void)size; }
static void panic_host(void) { fprintf(stderr, "linuxdrv: Linux panicked\n"); _exit(70); }

static void jmp_buf_set(struct lkl_jmp_buf *j, void (*f)(void)) { if (!setjmp(*(jmp_buf *)j->buf)) f(); }
static void jmp_buf_longjmp(struct lkl_jmp_buf *j, int v) { longjmp(*(jmp_buf *)j->buf, v); }

/* ---- PCI ---- */
struct lkl_pci_dev { unsigned bdf; int irq; volatile int quit; pthread_t th; };
struct bar { struct lkl_pci_dev *dev; volatile unsigned char *va; unsigned long size; };

static int cfg(struct lkl_pci_dev *d, int where, int size, unsigned *v, int write) {
    return (int)syscall(SYS_PCI_CONFIG, d->bdf, where, size, v, write);
}
static struct lkl_pci_dev *pci_add(const char *name, void *ram, unsigned long size) {
    unsigned bus, dev, fn;
    if (sscanf(name, "qrt%x:%x.%x", &bus, &dev, &fn) != 3) return NULL;
    struct lkl_pci_dev *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->bdf = bus << 8 | dev << 3 | fn;
    long e = syscall(SYS_PCI_CLAIM, d->bdf);
    if (e) { fprintf(stderr, "linuxdrv: could not claim %s (%ld)\n", name + 3, e); free(d); return NULL; }
    return d;
}
static void *irq_thread(void *p) {
    struct lkl_pci_dev *d = p;
    while (!d->quit) {
        unsigned st = 0;
        if (cfg(d, 6, 2, &st, 0) == 2 && (st & 8)) lkl_trigger_irq(d->irq);   /* Interrupt Status */
        usleep(1000);
    }
    return NULL;
}
static int pci_irq_init(struct lkl_pci_dev *d, int irq) {
    d->irq = irq;
    return pthread_create(&d->th, NULL, irq_thread, d) ? -1 : 0;
}
static void pci_remove(struct lkl_pci_dev *d) { d->quit = 1; pthread_join(d->th, NULL); free(d); }
static int pci_msi_init(struct lkl_pci_dev *d, int type, int nvec, int *irqs) { (void)d; (void)type; (void)nvec; (void)irqs; return -1; }
static void pci_msi_teardown(struct lkl_pci_dev *d, int type) { (void)d; (void)type; }
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
    if (v < ram_virt) { fprintf(stderr, "linuxdrv: DMA from outside Linux's RAM (%p)\n", vaddr); return 0; }
    return ram_phys + (v - ram_virt);
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
    .ioremap = lkl_ioremap, .iomem_access = lkl_iomem_access,
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

/* ---- the bridge: eth0's frames <-> a network interface in Tessera ---- */
#define SYS_NET_ATTACH 1045
#define SYS_NET_RX     1046
#define SYS_NET_TX     1047
struct sll { unsigned short family, protocol; int ifindex; unsigned short hatype; unsigned char pkttype, halen, addr[8]; };
static int pkt = -1;
static long net_handle = -1;

static void *bridge_rx(void *arg) {
    (void)arg;
    static unsigned char f[2048];
    for (;;) {
        long n = lkl_sys_read(pkt, (char *)f, sizeof f);
        if (n > 0 && n <= 1600) syscall(SYS_NET_RX, net_handle, f, n);
        else if (n < 0 && n != -LKL_EINTR) { fprintf(stderr, "linuxdrv: eth0 read: %s\n", lkl_strerror((int)n)); usleep(100000); }
    }
    return NULL;
}
static void *bridge_tx(void *arg) {
    (void)arg;
    static unsigned char f[2048];
    for (;;) {
        long n = syscall(SYS_NET_TX, net_handle, f, sizeof f, 200);
        if (n > 0) lkl_sys_write(pkt, (char *)f, n);
    }
    return NULL;
}
static int bridge(const char *ifname) {
    long s = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct lkl_ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.lkl_ifr_name, ifname, LKL_IFNAMSIZ - 1);
    if (lkl_sys_ioctl((int)s, LKL_SIOCGIFINDEX, (long)&ifr) < 0) { printf("linuxdrv: no %s: the driver did not create a network interface\n", ifname); return -1; }
    int ifindex = ifr.lkl_ifr_ifindex;
    unsigned char mac[6];
    lkl_sys_ioctl((int)s, LKL_SIOCGIFHWADDR, (long)&ifr);
    memcpy(mac, ifr.lkl_ifr_hwaddr.sa_data, 6);
    struct { unsigned cmd, data; } ev = { 0x2c, 0 };            /* ETHTOOL_SGRO: off - one frame in, one frame out */
    ifr.lkl_ifr_data = &ev;
    lkl_sys_ioctl((int)s, LKL_SIOCETHTOOL, (long)&ifr);
    lkl_sys_ioctl((int)s, LKL_SIOCGIFFLAGS, (long)&ifr);
    ifr.lkl_ifr_flags |= LKL_IFF_UP | LKL_IFF_PROMISC;
    if (lkl_sys_ioctl((int)s, LKL_SIOCSIFFLAGS, (long)&ifr) < 0) { printf("linuxdrv: %s did not come up\n", ifname); return -1; }
    lkl_sys_close((int)s);
    pkt = (int)lkl_sys_socket(LKL_AF_PACKET, LKL_SOCK_RAW, be16(0x0003));   /* ETH_P_ALL */
    if (pkt < 0) { printf("linuxdrv: packet socket: %s\n", lkl_strerror(pkt)); return -1; }
    struct sll a = { .family = LKL_AF_PACKET, .protocol = be16(0x0003), .ifindex = ifindex };
    if (lkl_sys_bind(pkt, (struct lkl_sockaddr *)&a, sizeof a) < 0) { printf("linuxdrv: bind to %s failed\n", ifname); return -1; }
    net_handle = syscall(SYS_NET_ATTACH, mac);
    if (net_handle < 0) { printf("linuxdrv: Tessera refused the network interface (%d)\n", errno); return -1; }
    pthread_t t1, t2;
    pthread_create(&t1, NULL, bridge_rx, NULL);
    pthread_create(&t2, NULL, bridge_tx, NULL);
    printf("linuxdrv: %s (%02x:%02x:%02x:%02x:%02x:%02x) bridged into Tessera's network stack\n", ifname,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: linuxdrv <bus:dev.fn> [--test host:port] [Linux kernel arguments]\n");
        return 2;
    }
    const char *test = NULL;
    char extra[512] = "";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--test") && i + 1 < argc) { test = argv[++i]; continue; }
        strncat(extra, " ", sizeof extra - strlen(extra) - 1);
        strncat(extra, argv[i], sizeof extra - strlen(extra) - 1);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (lkl_init(&lkl_host_ops) < 0) { fprintf(stderr, "linuxdrv: lkl_init failed\n"); return 1; }
    char cmd[768];
    snprintf(cmd, sizeof cmd, "mem=48M loglevel=6 lkl_pci=qrt%s %s%s", argv[1], test ? "ip=dhcp" : "ipv6.disable=1", extra);
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
    bridge("eth0");                                             /* a network card: into Tessera's stack */
    for (;;) pause();                                           /* the driver host stays up */
}
