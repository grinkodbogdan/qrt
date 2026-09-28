/*
 * mm.c - memory management once the firmware is gone.
 *
 * Physical frames come from the UEFI memory map: conventional memory plus
 * everything the firmware's boot services used.  Two pools:
 *   high (>= 1 GiB)  kernel heap, stacks, page tables - anything the kernel
 *                    touches while a user process's address space is live
 *   low  (< 1 GiB)   frames that back user pages
 * The first GiB of every process's address space belongs to the process, so
 * kernel data must never live there; the kernel only touches low frames with
 * its own page tables loaded.
 *
 * Kernel page tables identity-map all of physical memory with 2 MiB pages.
 * The framebuffer is mapped write-combining (PAT entry 1).
 */
#include "mm.h"

#define MAX_RANGES 128
typedef struct { u64 cur, end; } range_t;
static range_t low_r[MAX_RANGES], high_r[MAX_RANGES];
static int n_low, n_high;
static u64 *freed;           /* stack of released frames */
static usize n_freed, cap_freed;
static u64 total_bytes, used_frames;
static u64 kpml4_phys, max_phys;

u64 kernel_cr3(void) { return kpml4_phys; }
u64 mm_max_phys(void) { return max_phys; }
u64 pmm_total_bytes(void) { return total_bytes; }
u64 pmm_free_bytes(void) { return total_bytes - used_frames * PAGE; }

static int usable(u32 type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode || type == EfiBootServicesData;
}

static void add_range(u64 base, u64 end) {
    base = (base + PAGE - 1) & ~(PAGE - 1);
    end &= ~(PAGE - 1);
    if (base < 0x100000) base = 0x100000;          /* keep real-mode memory for the AP trampoline */
    if (end <= base) return;
    if (base < HIGH_POOL && end > HIGH_POOL) { add_range(base, HIGH_POOL); add_range(HIGH_POOL, end); return; }
    total_bytes += end - base;
    if (base >= HIGH_POOL) { if (n_high < MAX_RANGES) high_r[n_high++] = (range_t){ base, end }; }
    else if (n_low < MAX_RANGES) low_r[n_low++] = (range_t){ base, end };
}

/* Zero a frame through the identity map.  Low frames are shadowed by user
 * space in process address spaces, so borrow the kernel's tables for them. */
static void zero_frame(u64 f, usize bytes) {
    u64 cr3 = read_cr3();
    int swap = f < USER_TOP && kpml4_phys && cr3 != kpml4_phys;
    if (swap) write_cr3(kpml4_phys);
    memset((void *)(usize)f, 0, bytes);
    if (swap) write_cr3(cr3);
}

static u64 bump(range_t *r, int n, usize pages) {
    for (int i = 0; i < n; i++)
        if (r[i].end - r[i].cur >= pages * PAGE) {
            u64 f = r[i].cur;
            r[i].cur += pages * PAGE;
            return f;
        }
    return 0;
}

u64 pmm_alloc(int high) {
    u64 flags = irq_save();
    u64 f = 0;
    for (usize i = n_freed; i > 0 && !f; i--)          /* reuse a released frame of the right pool */
        if (!high || freed[i - 1] >= HIGH_POOL) { f = freed[i - 1]; freed[i - 1] = freed[--n_freed]; }
    if (!f && !high) f = bump(low_r, n_low, 1);
    if (!f) f = bump(high_r, n_high, 1);
    if (f) used_frames++;
    irq_restore(flags);
    if (!f) panic("out of physical memory");
    zero_frame(f, PAGE);
    return f;
}

u64 pmm_alloc_contig(usize pages) {
    u64 flags = irq_save();
    u64 f = bump(high_r, n_high, pages);
    if (f) used_frames += pages;
    irq_restore(flags);
    if (!f) panic("out of contiguous memory");
    zero_frame(f, pages * PAGE);
    return f;
}

void pmm_free(u64 f) {
    if (!f) return;
    u64 flags = irq_save();
    if (n_freed == cap_freed) {
        usize ncap = cap_freed ? cap_freed * 2 : 4096;
        u64 *nf = heap_alloc(ncap * sizeof(u64));
        if (freed) { memcpy(nf, freed, n_freed * sizeof(u64)); heap_free(freed); }
        freed = nf;
        cap_freed = ncap;
    }
    freed[n_freed++] = f;
    used_frames--;
    irq_restore(flags);
}

/* ---- kernel heap: first-fit free list with coalescing ------------------- */
typedef struct blk { usize size; struct blk *next; } blk_t;   /* size includes header, multiple of 16 */
#define MAX_ARENAS 32
static struct { u64 start, end; } arenas[MAX_ARENAS];
static int n_arenas;
static blk_t *free_list;

static void heap_insert(blk_t *b) {
    blk_t **pp = &free_list;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
    /* merge with the next block, then with the previous one */
    if (b->next && (u8 *)b + b->size == (u8 *)b->next) { b->size += b->next->size; b->next = b->next->next; }
    if (pp != &free_list) {
        blk_t *prev = (blk_t *)((u8 *)pp - __builtin_offsetof(blk_t, next));
        if ((u8 *)prev + prev->size == (u8 *)b) { prev->size += b->size; prev->next = b->next; }
    }
}

static void heap_grow(usize need) {
    usize bytes = MAX(need + PAGE, (usize)32 << 20);
    bytes = (bytes + PAGE - 1) & ~(PAGE - 1);
    if (n_arenas == MAX_ARENAS) panic("heap: too many arenas");
    u64 base = pmm_alloc_contig(bytes / PAGE);
    arenas[n_arenas].start = base;
    arenas[n_arenas].end = base + bytes;
    n_arenas++;
    blk_t *b = (blk_t *)(usize)base;
    b->size = bytes;
    heap_insert(b);
}

void *heap_alloc(usize n) {
    usize need = ((n + sizeof(blk_t) + 15) & ~(usize)15);
    u64 flags = irq_save();
    for (int attempt = 0; attempt < 2; attempt++) {
        for (blk_t **pp = &free_list; *pp; pp = &(*pp)->next) {
            blk_t *b = *pp;
            if (b->size < need) continue;
            if (b->size - need >= 64) {                 /* split */
                blk_t *rest = (blk_t *)((u8 *)b + need);
                rest->size = b->size - need;
                rest->next = b->next;
                *pp = rest;
                b->size = need;
            } else {
                *pp = b->next;
            }
            irq_restore(flags);
            void *p = (u8 *)b + sizeof(blk_t);
            memset(p, 0, b->size - sizeof(blk_t));
            return p;
        }
        heap_grow(need);
    }
    irq_restore(flags);
    panic("heap exhausted");
    return NULL;
}

void heap_free(void *p) {
    if (!p) return;
    u64 flags = irq_save();
    heap_insert((blk_t *)((u8 *)p - sizeof(blk_t)));
    irq_restore(flags);
}

int heap_owns(const void *p) {
    u64 a = (u64)(usize)p;
    for (int i = 0; i < n_arenas; i++) if (a >= arenas[i].start && a < arenas[i].end) return 1;
    return 0;
}

/* ---- page tables ----------------------------------------------------------- */
#define PTE_P   0x001ull
#define PTE_W   0x002ull
#define PTE_U   0x004ull
#define PTE_PWT 0x008ull
#define PTE_PS  0x080ull
#define PTE_ADDR 0x000ffffffffff000ull

void mm_init(const mm_boot_t *b, const u64 *reserve, int nreserve) {
    max_phys = 4ull << 30;
    for (usize off = 0; off < b->map_size; off += b->desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((u8 *)b->map + off);
        u64 end = d->PhysicalStart + d->NumberOfPages * PAGE;
        if (end > max_phys) max_phys = end;
        if (!usable(d->Type)) continue;
        int skip = 0;           /* never hand out the stack/tables we are still running on */
        for (int r = 0; r < nreserve; r++)
            if (reserve[r] >= d->PhysicalStart && reserve[r] < end) skip = 1;
        if (!skip) add_range(d->PhysicalStart, end);
    }
    if (b->fb_base + b->fb_size > max_phys) max_phys = b->fb_base + b->fb_size;
    if (max_phys > (64ull << 30)) max_phys = 64ull << 30;
    max_phys = (max_phys + (1ull << 30) - 1) & ~((1ull << 30) - 1);

    /* PAT entry 1: write-through -> write-combining (for the framebuffer) */
    wrmsr(MSR_PAT, (rdmsr(MSR_PAT) & ~0xff00ull) | 0x0100ull);

    u64 *pml4 = (u64 *)(usize)pmm_alloc(1);
    u64 *pdpt = (u64 *)(usize)pmm_alloc(1);
    for (u64 gb = 0; gb < (max_phys >> 30); gb++) {
        u64 *pd = (u64 *)(usize)pmm_alloc(1);
        for (u64 i = 0; i < 512; i++) {
            u64 a = (gb << 30) | (i << 21);
            u64 e = a | PTE_P | PTE_W | PTE_PS;
            if (b->fb_size && a + (2ull << 20) > b->fb_base && a < b->fb_base + b->fb_size) e |= PTE_PWT;
            pd[i] = e;
        }
        pdpt[gb] = (u64)(usize)pd | PTE_P | PTE_W;
    }
    pml4[0] = (u64)(usize)pdpt | PTE_P | PTE_W;
    kpml4_phys = (u64)(usize)pml4;
    write_cr3(kpml4_phys);
}

/* ---- user address spaces --------------------------------------------------- */
static u64 *tbl(u64 e) { return (u64 *)(usize)(e & PTE_ADDR); }

u64 as_create(void) {
    u64 *k4 = tbl(kpml4_phys), *kp = tbl(k4[0]);
    u64 *pml4 = (u64 *)(usize)pmm_alloc(1), *pdpt = (u64 *)(usize)pmm_alloc(1);
    memcpy(pml4, k4, PAGE);
    memcpy(pdpt, kp, PAGE);
    pdpt[0] = pmm_alloc(1) | PTE_P | PTE_W | PTE_U;      /* the user's first GiB */
    pml4[0] = (u64)(usize)pdpt | PTE_P | PTE_W | PTE_U;
    return (u64)(usize)pml4;
}

static u64 *user_pd(u64 cr3) { return tbl(tbl(tbl(cr3)[0])[0]); }

int as_map(u64 cr3, u64 va, u64 frame, int writable) {
    if (va >= USER_TOP) return -1;
    u64 *pd = user_pd(cr3);
    u64 *pde = &pd[(va >> 21) & 511];
    if (!(*pde & PTE_P)) *pde = pmm_alloc(1) | PTE_P | PTE_W | PTE_U;
    u64 *pt = tbl(*pde);
    pt[(va >> 12) & 511] = (frame & PTE_ADDR) | PTE_P | PTE_U | (writable ? PTE_W : 0);
    if (read_cr3() == cr3) __asm__ volatile("invlpg (%0)" : : "r"((usize)va) : "memory");
    return 0;
}

u64 as_translate(u64 cr3, u64 va) {
    if (va >= USER_TOP) return 0;
    u64 pde = user_pd(cr3)[(va >> 21) & 511];
    if (!(pde & PTE_P)) return 0;
    u64 pte = tbl(pde)[(va >> 12) & 511];
    return (pte & PTE_P) ? (pte & PTE_ADDR) | (va & 0xfff) : 0;
}

void as_unmap(u64 cr3, u64 va) {
    u64 pde = user_pd(cr3)[(va >> 21) & 511];
    if (!(pde & PTE_P)) return;
    u64 *pte = &tbl(pde)[(va >> 12) & 511];
    if (*pte & PTE_P) { pmm_free(*pte & PTE_ADDR); *pte = 0; }
    if (read_cr3() == cr3) __asm__ volatile("invlpg (%0)" : : "r"((usize)va) : "memory");
}

void as_destroy(u64 cr3) {
    u64 *pml4 = tbl(cr3), *pdpt = tbl(pml4[0]), *pd = tbl(pdpt[0]);
    for (int i = 0; i < 512; i++) {
        if (!(pd[i] & PTE_P)) continue;
        u64 *pt = tbl(pd[i]);
        for (int j = 0; j < 512; j++) if (pt[j] & PTE_P) pmm_free(pt[j] & PTE_ADDR);
        pmm_free(pd[i] & PTE_ADDR);
    }
    pmm_free(pdpt[0] & PTE_ADDR);
    pmm_free(pml4[0] & PTE_ADDR);
    pmm_free(cr3);
}
