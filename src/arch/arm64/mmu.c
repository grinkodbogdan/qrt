/*
 * mmu.c - memory on 64-bit ARM: an identity map (virtual = physical), RAM write-back
 * cacheable, the framebuffer uncached (the display engine reads memory without
 * snooping the caches), everything else device memory; a page allocator over the free
 * RAM; the kernel heap (the same first-fit list as the x86-64 kernel's).
 *
 * 4 KB granule, 39-bit addresses: one level-1 table of 1 GB blocks, with a level-2
 * table of 2 MB blocks where the framebuffer needs a different memory type.
 */
#include "arm.h"

#define MAIR_DEVICE 0      /* Device-nGnRE */
#define MAIR_NORMAL 1      /* Normal, write-back */
#define MAIR_NC     2      /* Normal, non-cacheable */
#define D_BLOCK  1ull
#define D_TABLE  3ull
#define D_AF     (1ull << 10)
#define D_ISH    (3ull << 8)
#define D_PXN    (1ull << 53)
#define D_UXN    (1ull << 54)
#define D_ATTR(i) ((u64)(i) << 2)

#define NL2 16
static u64 l1[512] __attribute__((aligned(4096)));
static u64 l2[NL2][512] __attribute__((aligned(4096)));

static u64 block(u64 pa, int attr) {
    u64 d = pa | D_BLOCK | D_AF | D_ATTR(attr);
    if (attr == MAIR_DEVICE) d |= D_PXN | D_UXN;
    else d |= D_ISH;
    return d;
}

static int overlaps(u64 a, u64 b, const u64 (*r)[2], int n) {
    for (int i = 0; i < n; i++) if (a < r[i][0] + r[i][1] && b > r[i][0]) return 1;
    return 0;
}
static int inside(u64 a, u64 b, const u64 (*r)[2], int n) {
    for (int i = 0; i < n; i++) if (a >= r[i][0] && b <= r[i][0] + r[i][1]) return 1;
    return 0;
}

static void dcache_inval(u64 start, u64 end) {
    for (u64 a = start & ~63ull; a < end; a += 64) __asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

extern char _start[], _image_size[];

/* RAM: 2 MB blocks wholly inside a bank and clear of the firmware's reserved regions
 * (no speculative reads into memory the secure world protects); the framebuffer and
 * the DMA pool (nc) uncached; everything else device memory */
void mmu_init(const u64 (*ram)[2], int nram, const u64 (*fbr)[2], int nfb, const u64 (*hole)[2], int nhole) {
    u64 img[1][2] = { { (u64)(usize)_start, (u64)(usize)_image_size } };   /* always runnable */
    int nl2 = 0;
    for (u64 g = 0; g < 512; g++) {
        u64 lo = g << 30, hi = lo + (1ull << 30);
        if (!overlaps(lo, hi, ram, nram) && !overlaps(lo, hi, img, 1) && !overlaps(lo, hi, fbr, nfb)) { l1[g] = block(lo, MAIR_DEVICE); continue; }
        if (nl2 == NL2) { l1[g] = block(lo, MAIR_DEVICE); continue; }
        u64 *t = l2[nl2++];
        for (u64 i = 0; i < 512; i++) {
            u64 a = lo + (i << 21), b = a + (1ull << 21);
            int attr = MAIR_DEVICE;
            if (overlaps(a, b, img, 1)) attr = MAIR_NORMAL;
            else if (overlaps(a, b, fbr, nfb)) attr = MAIR_NC;
            else if (inside(a, b, ram, nram) && !overlaps(a, b, hole, nhole)) attr = MAIR_NORMAL;
            t[i] = block(a, attr);
        }
        l1[g] = (u64)(usize)t | D_TABLE;
    }
    /* RAM the boot loader may have cached before we wrote it with the caches off */
    u64 base = (u64)(usize)_start;
    dcache_inval(base, base + (u64)(usize)_image_size);
    SYSREG_W(mair_el1, 0x04ull << (8 * MAIR_DEVICE) | 0xffull << (8 * MAIR_NORMAL) | 0x44ull << (8 * MAIR_NC));
    u64 pa_range = SYSREG_R(id_aa64mmfr0_el1) & 7;
    u64 tcr = 25 | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (pa_range << 32);   /* T0SZ 25, WB, ISH, 4K, EPD1 */
    SYSREG_W(tcr_el1, tcr);
    SYSREG_W(ttbr0_el1, (u64)(usize)l1);
    __asm__ volatile("dsb sy; tlbi vmalle1; dsb sy; isb" ::: "memory");
    u64 sctlr = SYSREG_R(sctlr_el1);
    sctlr |= 1 | (1 << 2) | (1 << 12);          /* MMU, data cache, instruction cache */
    sctlr &= ~(u64)((1 << 1) | (1 << 3) | (1 << 4) | (1 << 19));   /* no alignment check, no SP alignment check, no WXN */
    SYSREG_W(sctlr_el1, sctlr);
    __asm__ volatile("isb" ::: "memory");
}

/* ---- pages: a bump allocator over the free RAM span, with a free list ----------- */
static u64 pool_next, pool_end;
static u64 *free_pages;                       /* singly linked through the pages themselves */
void pmm_init(u64 base, u64 end) { pool_next = (base + 4095) & ~4095ull; pool_end = end & ~4095ull; }
u64 pmm_free_bytes(void) { return pool_end > pool_next ? pool_end - pool_next : 0; }
u64 pmm_alloc_contig(usize pages) {
    u64 bytes = (u64)pages * 4096;
    u64 f = irq_save();
    if (pool_next + bytes > pool_end) { irq_restore(f); return 0; }
    u64 p = pool_next;
    pool_next += bytes;
    irq_restore(f);
    memset((void *)(usize)p, 0, bytes);
    return p;
}
u64 pmm_alloc(int high) {
    (void)high;
    u64 f = irq_save();
    if (free_pages) {
        u64 *p = free_pages;
        free_pages = (u64 *)(usize)*p;
        irq_restore(f);
        memset(p, 0, 4096);
        return (u64)(usize)p;
    }
    irq_restore(f);
    return pmm_alloc_contig(1);
}
void pmm_free(u64 frame) {
    u64 f = irq_save();
    *(u64 *)(usize)frame = (u64)(usize)free_pages;
    free_pages = (u64 *)(usize)frame;
    irq_restore(f);
}

/* ---- kernel heap: first-fit free list with coalescing (as src/arch/x64/mm.c) ---- */
typedef struct blk { usize size; struct blk *next; } blk_t;
#define MAX_ARENAS 32
static struct { u64 start, end; } arenas[MAX_ARENAS];
static int n_arenas;
static blk_t *free_list;

static void heap_insert(blk_t *b) {
    blk_t **pp = &free_list;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
    if (b->next && (u8 *)b + b->size == (u8 *)b->next) { b->size += b->next->size; b->next = b->next->next; }
    if (pp != &free_list) {
        blk_t *prev = (blk_t *)((u8 *)pp - __builtin_offsetof(blk_t, next));
        if ((u8 *)prev + prev->size == (u8 *)b) { prev->size += b->size; prev->next = b->next; }
    }
}

static void heap_grow(usize need) {
    usize bytes = MAX(need + 4096, (usize)32 << 20);
    bytes = (bytes + 4095) & ~(usize)4095;
    if (n_arenas == MAX_ARENAS) panic("heap: too many arenas");
    u64 base = pmm_alloc_contig(bytes / 4096);
    if (!base) panic("out of memory");
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
            if (b->size - need >= 64) {
                blk_t *rest = (blk_t *)((u8 *)b + need);
                rest->size = b->size - need;
                rest->next = b->next;
                *pp = rest;
                b->size = need;
            } else *pp = b->next;
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
