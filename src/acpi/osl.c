/*
 * osl.c - the operating-system layer ACPICA calls (acpiosxf.h), for QRT's native kernel.
 *
 * As Linux's drivers/acpi/osl.c, reduced to what QRT needs: physical memory is
 * identity-mapped (high MMIO through mm_map_mmio), ports and PCI configuration space
 * directly, kernel threads sleep while a semaphore is taken.  ACPICA runs in one
 * kernel thread (src/acpi/acpi.c), which also does the deferred work ACPICA hands to
 * AcpiOsExecute and calls the SCI handler on every tick instead of taking the
 * interrupt (the SCI line stays masked).
 *
 * Only ACPICA's headers are included here; the kernel functions used are declared below.
 */
#include "acpi.h"

/* ---- the kernel, without its headers (their types clash with ACPICA's) ---- */
typedef unsigned long long kq;
extern void *kalloc(kq n);
extern void kfree(void *p);
extern void klog(const char *f, ...);
extern kq k_now_us(void);
extern kq k_now_ms(void);
extern void hal_delay_us(unsigned int us);
extern void *thread_current(void);
extern void thread_sleep_ms(kq ms);
extern unsigned int pci_read32(unsigned char bus, unsigned char dev, unsigned char fn, unsigned short off);
extern void pci_write32(unsigned char bus, unsigned char dev, unsigned char fn, unsigned short off, unsigned int v);
extern kq mm_max_phys(void);
extern void *mm_map_mmio(kq base, kq size);
extern const unsigned char *acpi_rsdp(void);
int vsnprintf(char *s, ACPI_SIZE n, const char *f, va_list ap);

static inline kq irqsave(void) { kq f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irqrestore(kq f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

/* ---- the C library functions the kernel does not have ---- */
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, unsigned long long n) {
    char *r = d;
    for (; n && *s; n--) *d++ = *s++;
    for (; n; n--) *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, unsigned long long n) {
    char *e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}
unsigned long strtoul(const char *s, char **end, int base) {
    unsigned long v = 0;
    while (isspace(*s)) s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = s[0] == '0' ? 8 : 10;
    for (;; s++) {
        int d = isdigit(*s) ? *s - '0' : isalpha(*s) ? tolower(*s) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)s;
    return v;
}

/* ---- start, tables ---- */
ACPI_STATUS AcpiOsInitialize(void) { return AE_OK; }
ACPI_STATUS AcpiOsTerminate(void) { return AE_OK; }
ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void) { return (ACPI_PHYSICAL_ADDRESS)(kq)acpi_rsdp(); }
ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *o, ACPI_STRING *n) { *n = NULL; return AE_OK; }
ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *e, ACPI_TABLE_HEADER **n) { *n = NULL; return AE_OK; }
ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *e, ACPI_PHYSICAL_ADDRESS *a, UINT32 *l) { *a = 0; return AE_OK; }

/* ---- memory ---- */
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS p, ACPI_SIZE len) {
    if (p + len <= mm_max_phys()) return (void *)(kq)p;
    return mm_map_mmio(p, len);
}
void AcpiOsUnmapMemory(void *v, ACPI_SIZE len) {}
ACPI_STATUS AcpiOsGetPhysicalAddress(void *v, ACPI_PHYSICAL_ADDRESS *p) { *p = (ACPI_PHYSICAL_ADDRESS)(kq)v; return AE_OK; }
void *AcpiOsAllocate(ACPI_SIZE n) { return kalloc(n ? n : 1); }
void AcpiOsFree(void *p) { kfree(p); }

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS a, UINT64 *v, UINT32 w) {
    volatile void *p = AcpiOsMapMemory(a, w / 8);
    switch (w) {
    case 8: *v = *(volatile UINT8 *)p; break;
    case 16: *v = *(volatile UINT16 *)p; break;
    case 32: *v = *(volatile UINT32 *)p; break;
    case 64: *v = *(volatile UINT64 *)p; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}
ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS a, UINT64 v, UINT32 w) {
    volatile void *p = AcpiOsMapMemory(a, w / 8);
    switch (w) {
    case 8: *(volatile UINT8 *)p = (UINT8)v; break;
    case 16: *(volatile UINT16 *)p = (UINT16)v; break;
    case 32: *(volatile UINT32 *)p = (UINT32)v; break;
    case 64: *(volatile UINT64 *)p = v; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

/* ---- ports, PCI ---- */
ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS port, UINT32 *v, UINT32 w) {
    UINT16 p = (UINT16)port;
    if (w == 8) { UINT8 b; __asm__ volatile("inb %1, %0" : "=a"(b) : "Nd"(p)); *v = b; }
    else if (w == 16) { UINT16 x; __asm__ volatile("inw %1, %0" : "=a"(x) : "Nd"(p)); *v = x; }
    else if (w == 32) { UINT32 x; __asm__ volatile("inl %1, %0" : "=a"(x) : "Nd"(p)); *v = x; }
    else return AE_BAD_PARAMETER;
    return AE_OK;
}
ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS port, UINT32 v, UINT32 w) {
    UINT16 p = (UINT16)port;
    if (w == 8) __asm__ volatile("outb %0, %1" : : "a"((UINT8)v), "Nd"(p));
    else if (w == 16) __asm__ volatile("outw %0, %1" : : "a"((UINT16)v), "Nd"(p));
    else if (w == 32) __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p));
    else return AE_BAD_PARAMETER;
    return AE_OK;
}
ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *id, UINT32 reg, UINT64 *v, UINT32 w) {
    if (id->Segment || reg > 0xfff) { *v = 0; return AE_OK; }
    UINT32 d = pci_read32((UINT8)id->Bus, (UINT8)id->Device, (UINT8)id->Function, (UINT16)(reg & ~3u));
    d >>= (reg & 3) * 8;
    *v = w == 8 ? (d & 0xff) : w == 16 ? (d & 0xffff) : d;
    if (w == 64) *v |= (UINT64)pci_read32((UINT8)id->Bus, (UINT8)id->Device, (UINT8)id->Function, (UINT16)(reg + 4)) << 32;
    return AE_OK;
}
ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *id, UINT32 reg, UINT64 v, UINT32 w) {
    if (id->Segment || reg > 0xfff) return AE_OK;
    UINT8 b = (UINT8)id->Bus, dv = (UINT8)id->Device, fn = (UINT8)id->Function;
    if (w >= 32) {
        pci_write32(b, dv, fn, (UINT16)reg, (UINT32)v);
        if (w == 64) pci_write32(b, dv, fn, (UINT16)(reg + 4), (UINT32)(v >> 32));
        return AE_OK;
    }
    UINT32 sh = (reg & 3) * 8, mask = (w == 8 ? 0xffu : 0xffffu) << sh;
    UINT32 d = pci_read32(b, dv, fn, (UINT16)(reg & ~3u));
    pci_write32(b, dv, fn, (UINT16)(reg & ~3u), (d & ~mask) | (((UINT32)v << sh) & mask));
    return AE_OK;
}

/* ---- locks, semaphores ---- */
ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *l) { *l = kalloc(8); return AE_OK; }
void AcpiOsDeleteLock(ACPI_SPINLOCK l) { kfree(l); }
ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK l) {
    kq f = irqsave();
    while (__atomic_exchange_n((volatile int *)l, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return f;
}
void AcpiOsReleaseLock(ACPI_SPINLOCK l, ACPI_CPU_FLAGS f) {
    __atomic_store_n((volatile int *)l, 0, __ATOMIC_RELEASE);
    irqrestore(f);
}

typedef struct { volatile int lock; volatile UINT32 units, max; } sem_t;
ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max, UINT32 initial, ACPI_SEMAPHORE *out) {
    sem_t *s = kalloc(sizeof *s);
    s->units = initial; s->max = max;
    *out = s;
    return AE_OK;
}
ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE h) { kfree(h); return AE_OK; }
ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE h, UINT32 units, UINT16 timeout) {
    sem_t *s = h;
    kq deadline = k_now_ms() + timeout;
    for (;;) {
        kq f = AcpiOsAcquireLock((ACPI_SPINLOCK)&s->lock);
        int ok = s->units >= units;
        if (ok) s->units -= units;
        AcpiOsReleaseLock((ACPI_SPINLOCK)&s->lock, f);
        if (ok) return AE_OK;
        if (timeout == 0 || (timeout != ACPI_WAIT_FOREVER && k_now_ms() >= deadline)) return AE_TIME;
        thread_sleep_ms(1);
    }
}
ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE h, UINT32 units) {
    sem_t *s = h;
    kq f = AcpiOsAcquireLock((ACPI_SPINLOCK)&s->lock);
    s->units += units;
    AcpiOsReleaseLock((ACPI_SPINLOCK)&s->lock, f);
    return AE_OK;
}

/* the FACS global lock (Linux: __acpi_acquire_global_lock / __acpi_release_global_lock) */
int AcpiOsQrtAcquireGlobalLock(void *facs) {
    volatile UINT32 *lock = &((ACPI_TABLE_FACS *)facs)->GlobalLock;
    UINT32 old, new;
    do {
        old = *lock;
        new = ((old & ~3u) + 2) + ((old >> 1) & 1);
    } while (!__atomic_compare_exchange_n(lock, &old, new, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return new < 3 ? -1 : 0;
}
int AcpiOsQrtReleaseGlobalLock(void *facs) {
    volatile UINT32 *lock = &((ACPI_TABLE_FACS *)facs)->GlobalLock;
    UINT32 old, new;
    do {
        old = *lock;
        new = old & ~3u;
    } while (!__atomic_compare_exchange_n(lock, &old, new, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return (int)(old & 1);
}

/* ---- threads, time ---- */
ACPI_THREAD_ID AcpiOsGetThreadId(void) { return (ACPI_THREAD_ID)(kq)thread_current() | 1; }
void AcpiOsSleep(UINT64 ms) { thread_sleep_ms(ms); }
void AcpiOsStall(UINT32 us) { hal_delay_us(us); }
UINT64 AcpiOsGetTimer(void) { return k_now_us() * 10; }       /* 100 ns units */

/* deferred work: run by the ACPI thread (acpi.c calls AcpiOsQrtRunDeferred) */
#define NWORK 64
static struct { ACPI_OSD_EXEC_CALLBACK fn; void *ctx; } work[NWORK];
static volatile int work_head, work_tail, work_lock;
ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE type, ACPI_OSD_EXEC_CALLBACK fn, void *ctx) {
    kq f = AcpiOsAcquireLock((ACPI_SPINLOCK)&work_lock);
    int next = (work_tail + 1) % NWORK;
    if (next == work_head) { AcpiOsReleaseLock((ACPI_SPINLOCK)&work_lock, f); return AE_NO_MEMORY; }
    work[work_tail].fn = fn; work[work_tail].ctx = ctx;
    work_tail = next;
    AcpiOsReleaseLock((ACPI_SPINLOCK)&work_lock, f);
    return AE_OK;
}
void AcpiOsQrtRunDeferred(void) {
    for (;;) {
        kq f = AcpiOsAcquireLock((ACPI_SPINLOCK)&work_lock);
        if (work_head == work_tail) { AcpiOsReleaseLock((ACPI_SPINLOCK)&work_lock, f); return; }
        ACPI_OSD_EXEC_CALLBACK fn = work[work_head].fn;
        void *ctx = work[work_head].ctx;
        work_head = (work_head + 1) % NWORK;
        AcpiOsReleaseLock((ACPI_SPINLOCK)&work_lock, f);
        fn(ctx);
    }
}
void AcpiOsWaitEventsComplete(void) { AcpiOsQrtRunDeferred(); }

/* the SCI: polled (acpi.c calls AcpiOsQrtPollSci on every tick) */
static ACPI_OSD_HANDLER sci_fn;
static void *sci_ctx;
static UINT32 sci_irq;
ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 irq, ACPI_OSD_HANDLER fn, void *ctx) {
    sci_irq = irq; sci_ctx = ctx; sci_fn = fn;
    return AE_OK;
}
ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 irq, ACPI_OSD_HANDLER fn) { sci_fn = NULL; return AE_OK; }
UINT32 AcpiOsQrtPollSci(void) { return sci_fn ? sci_fn(sci_ctx) : 0; }
UINT32 AcpiOsQrtSciIrq(void) { return sci_irq; }

/* ---- output: whole lines to the kernel log ---- */
static char line[256];
static int line_len;
void AcpiOsVprintf(const char *f, va_list ap) {
    char tmp[256];
    vsnprintf(tmp, sizeof tmp, f, ap);
    for (char *p = tmp; *p; p++) {
        if (*p == '\n' || line_len == (int)sizeof line - 1) {
            line[line_len] = 0;
            if (line_len) klog("acpi: %s", line);
            line_len = 0;
            if (*p == '\n') continue;
        }
        line[line_len++] = *p;
    }
}
void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    AcpiOsVprintf(f, ap);
    va_end(ap);
}
void AcpiOsRedirectOutput(void *d) {}

ACPI_STATUS AcpiOsSignal(UINT32 fn, void *info) {
    if (fn == ACPI_SIGNAL_FATAL) klog("acpi: the firmware's AML reported a fatal error");
    return AE_OK;
}
ACPI_STATUS AcpiOsEnterSleep(UINT8 state, UINT32 a, UINT32 b) { return AE_OK; }
