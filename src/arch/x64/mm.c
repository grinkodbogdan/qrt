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
    u64 fl = irq_save();              /* a thread switch mid-way would reload the user's tables */
    u64 cr3 = read_cr3();
    int swap = f < USER_TOP && kpml4_phys && cr3 != kpml4_phys;
    if (swap) write_cr3(kpml4_phys);
    memset((void *)(usize)f, 0, bytes);
    if (swap) write_cr3(cr3);
    irq_restore(fl);
}

/* Copy into a frame by physical address (same caveat as zero_frame). */
void phys_write(u64 pa, const void *src, usize len) {
    u64 fl = irq_save();
    u64 cr3 = read_cr3();
    int swap = pa < USER_TOP && kpml4_phys && cr3 != kpml4_phys;
    if (swap) write_cr3(kpml4_phys);
    memcpy((void *)(usize)pa, src, len);
    if (swap) write_cr3(cr3);
    irq_restore(fl);
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
#define PTE_PCD 0x010ull
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

/* Map device registers that lie outside the identity map (64-bit BARs are
 * often placed far above RAM, e.g. at 512 GiB by OVMF): identity-mapped,
 * uncached, 2 MiB pages, in the kernel's tables.  Only kernel threads use
 * these addresses. */
void *mm_map_mmio(u64 base, u64 size) {
    if (!kpml4_phys || !size) return NULL;
    if (base + size <= max_phys) { mm_uncached(base, size); return (void *)(usize)base; }
    if (base + size > USER_HOLE_END) { klog("mm: device registers at %llx are beyond the kernel's window", base); return NULL; }
    u64 *pml4 = tbl(kpml4_phys);
    for (u64 a = base & ~((2ull << 20) - 1); a < base + size; a += 2ull << 20) {
        u64 *e4 = &pml4[(a >> 39) & 511];
        if (!(*e4 & PTE_P)) *e4 = pmm_alloc(1) | PTE_P | PTE_W;
        u64 *e3 = &tbl(*e4)[(a >> 30) & 511];
        if (!(*e3 & PTE_P)) *e3 = pmm_alloc(1) | PTE_P | PTE_W;
        tbl(*e3)[(a >> 21) & 511] = a | PTE_P | PTE_W | PTE_PS | PTE_PCD | PTE_PWT;
    }
    write_cr3(read_cr3());
    return (void *)(usize)base;
}


/* Map a device's registers uncached (PAT entry 3) in the kernel's identity
 * map.  The firmware's MTRRs normally make MMIO uncached anyway; this makes
 * it independent of them.  Whole 2 MiB pages are changed. */
void mm_uncached(u64 base, u64 size) {
    if (!kpml4_phys || base + size > max_phys) return;
    u64 *pdpt = tbl(tbl(kpml4_phys)[0]);
    for (u64 a = base & ~((2ull << 20) - 1); a < base + size; a += 2ull << 20) {
        u64 *pd = tbl(pdpt[a >> 30]);
        pd[(a >> 21) & 511] |= PTE_PCD | PTE_PWT;
    }
    write_cr3(read_cr3());
}


/* ---- user address spaces ---------------------------------------------------------
 * Two regions per process: the first GiB (pdpt[0]: classic ELF addresses, brk, the
 * stack) and [64 GiB, 128 TiB) for mmap (Ladybird's garbage collector reserves 4 TiB
 * cages): pdpt[64..511] under pml4[0], then pml4[2..255] with their own pdpts.  pml4[1]
 * ([512 GiB, 1 TiB)) stays the kernel's, for device registers mapped above RAM.  The
 * identity map stops at 64 GiB of physical addresses, so the high region never shadows
 * kernel memory.  Every user entry carries PTE_U; kernel ones do not.
 * PTE bit 9 (free for software) marks frames of shared objects (memfd, MAP_SHARED):
 * they belong to the object, not to the mapping. */
#define PTE_SOFT_SHARED 0x200ull
#define PTE_SOFT_PARKED 0x400ull       /* PROT_NONE: not present, but the frame (and its contents) is kept */
#define PTE_HAS(e) ((e) & (PTE_P | PTE_SOFT_PARKED))
#define PTE_NX (1ull << 63)
static u64 nx_bit;

void mm_enable_nx(void) {
    u32 r[4];
    cpuid(0x80000001, 0, r);
    if (!(r[3] & (1u << 20))) return;
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1ull << 11));
    nx_bit = PTE_NX;
}

int user_va(u64 va) {
    return va < USER_TOP || (va >= USER_HIGH_BASE && va < USER_HOLE_BASE) || (va >= USER_HOLE_END && va < USER_HIGH_END);
}
int user_high_range(u64 start, u64 end) {
    return (start >= USER_HIGH_BASE && end <= USER_HOLE_BASE) || (start >= USER_HOLE_END && end <= USER_HIGH_END);
}
/* the next user address at or after va that is in a region (0: none) */
static u64 next_user(u64 va) { return va < USER_HIGH_BASE ? USER_HIGH_BASE : va < USER_HOLE_END ? USER_HOLE_END : 0; }

u64 as_create(void) {
    u64 *k4 = tbl(kpml4_phys), *kp = tbl(k4[0]);
    u64 *pml4 = (u64 *)(usize)pmm_alloc(1), *pdpt = (u64 *)(usize)pmm_alloc(1);
    memcpy(pml4, k4, PAGE);
    memcpy(pdpt, kp, PAGE);
    pdpt[0] = pmm_alloc(1) | PTE_P | PTE_W | PTE_U;      /* the user's first GiB */
    pml4[0] = (u64)(usize)pdpt | PTE_P | PTE_W | PTE_U;
    return (u64)(usize)pml4;
}

#define PML4_USER_FIRST 2                 /* pml4[2..255]: [1 TiB, 128 TiB) */
#define PML4_USER_END   256

/* the pdpt that covers va (pml4[0]'s is shared with the kernel's entries); create: make it */
static u64 *pdpt_of(u64 cr3, u64 va, int create) {
    u64 *e4 = &tbl(cr3)[(va >> 39) & 511];
    if (!(*e4 & PTE_P)) { if (!create) return NULL; *e4 = pmm_alloc(1) | PTE_P | PTE_W | PTE_U; }
    return tbl(*e4);
}

/* the page table entry for va, or NULL; create: build the missing levels */
static u64 *pte_of(u64 cr3, u64 va, int create) {
    if (!user_va(va)) return NULL;
    u64 *p3 = pdpt_of(cr3, va, create);
    if (!p3) return NULL;
    u64 *e3 = &p3[(va >> 30) & 511];
    if (!(*e3 & PTE_P)) { if (!create) return NULL; *e3 = pmm_alloc(1) | PTE_P | PTE_W | PTE_U; }
    else if (!(*e3 & PTE_U)) return NULL;                 /* the kernel's (a device mapped up here) */
    u64 *e2 = &tbl(*e3)[(va >> 21) & 511];
    if (!(*e2 & PTE_P)) { if (!create) return NULL; *e2 = pmm_alloc(1) | PTE_P | PTE_W | PTE_U; }
    return &tbl(*e2)[(va >> 12) & 511];
}

static u64 pte_bits(int prot) {
    return PTE_P | PTE_U | ((prot & AS_W) ? PTE_W : 0) | ((prot & AS_X) ? 0 : nx_bit) | ((prot & AS_SHARED) ? PTE_SOFT_SHARED : 0);
}

static void flush(u64 cr3, u64 va) { if (read_cr3() == cr3) __asm__ volatile("invlpg (%0)" : : "r"((usize)va) : "memory"); }

static void clone_pdpt(u64 *sp3, u64 *dp3) {
    for (int g = 0; g < 512; g++) {
        if (!(sp3[g] & PTE_P) || !(sp3[g] & PTE_U)) continue;
        u64 *spd = tbl(sp3[g]);
        if (!(dp3[g] & PTE_P)) dp3[g] = pmm_alloc(1) | PTE_P | PTE_W | PTE_U;
        u64 *dpd = tbl(dp3[g]);
        for (int i = 0; i < 512; i++) {
            if (!(spd[i] & PTE_P)) continue;
            u64 *spt = tbl(spd[i]);
            u64 dpt = pmm_alloc(1);
            dpd[i] = dpt | PTE_P | PTE_W | PTE_U;
            for (int j = 0; j < 512; j++) {
                u64 e = spt[j];
                if (!PTE_HAS(e)) continue;
                if (e & PTE_SOFT_SHARED) { tbl(dpt)[j] = e; continue; }   /* shared memory stays shared */
                u64 f = pmm_alloc(0);
                /* user frames are low: copy them with the kernel's tables loaded */
                u64 fl = irq_save(), cr3 = read_cr3();
                if (cr3 != kpml4_phys) write_cr3(kpml4_phys);
                memcpy((void *)(usize)f, (void *)(usize)(e & PTE_ADDR), PAGE);
                if (cr3 != kpml4_phys) write_cr3(cr3);
                irq_restore(fl);
                tbl(dpt)[j] = f | (e & ~PTE_ADDR);
            }
        }
    }
}

u64 as_clone(u64 src) {
    u64 dst = as_create();
    clone_pdpt(pdpt_of(src, 0, 0), pdpt_of(dst, 0, 0));
    for (int i = PML4_USER_FIRST; i < PML4_USER_END; i++)
        if (tbl(src)[i] & PTE_P) clone_pdpt(tbl(tbl(src)[i]), pdpt_of(dst, (u64)i << 39, 1));
    return dst;
}

int as_map(u64 cr3, u64 va, u64 frame, int prot) {
    u64 *pte = pte_of(cr3, va, 1);
    if (!pte) return -1;
    if (PTE_HAS(*pte) && !(*pte & PTE_SOFT_SHARED) && (*pte & PTE_ADDR) != (frame & PTE_ADDR)) pmm_free(*pte & PTE_ADDR);
    *pte = (frame & PTE_ADDR) | pte_bits(prot);
    flush(cr3, va);
    return 0;
}

u64 as_translate(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    return pte && (*pte & PTE_P) ? (*pte & PTE_ADDR) | (va & 0xfff) : 0;
}

int as_pte_writable(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    return pte && (*pte & PTE_P) && (*pte & PTE_W);
}

void as_unmap(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    if (!pte || !PTE_HAS(*pte)) return;
    if (!(*pte & PTE_SOFT_SHARED)) pmm_free(*pte & PTE_ADDR);
    *pte = 0;
    flush(cr3, va);
}

void as_protect(u64 cr3, u64 va, int prot) {
    u64 *pte = pte_of(cr3, va, 0);
    if (!pte || !PTE_HAS(*pte)) return;
    u64 keep = *pte & (PTE_ADDR | PTE_SOFT_SHARED);
    *pte = (prot & AS_NONE) ? keep | PTE_SOFT_PARKED : (*pte & PTE_ADDR) | pte_bits(prot | ((*pte & PTE_SOFT_SHARED) ? AS_SHARED : 0));
    flush(cr3, va);
}

int as_parked(u64 cr3, u64 va) { u64 *pte = pte_of(cr3, va, 0); return pte && (*pte & PTE_SOFT_PARKED) && !(*pte & PTE_P); }

int as_move(u64 cr3, u64 from, u64 to) {
    u64 *ps = pte_of(cr3, from, 0);
    if (!ps || !PTE_HAS(*ps)) return 0;
    u64 e = *ps;
    *ps = 0;
    flush(cr3, from);
    u64 *pd = pte_of(cr3, to, 1);
    if (!pd) { if (!(e & PTE_SOFT_SHARED)) pmm_free(e & PTE_ADDR); return 0; }
    *pd = e;
    flush(cr3, to);
    return 1;
}

/* walk the present (or parked) pages of [start, end), skipping empty tables in bulk */
static void as_walk(u64 cr3, u64 start, u64 end, void (*fn)(u64 cr3, u64 *pte, u64 va, int arg), int arg) {
    u64 va = start & ~(PAGE - 1);
    while (va < end) {
        if (!user_va(va)) { if (!(va = next_user(va))) break; continue; }
        u64 *p3 = pdpt_of(cr3, va, 0);
        if (!p3) { va = (va + (1ull << 39)) & ~((1ull << 39) - 1); continue; }
        u64 e3 = p3[(va >> 30) & 511];
        if (!(e3 & PTE_P) || !(e3 & PTE_U)) { va = (va + (1ull << 30)) & ~((1ull << 30) - 1); continue; }
        u64 e2 = tbl(e3)[(va >> 21) & 511];
        if (!(e2 & PTE_P)) { va = (va + (2ull << 20)) & ~((2ull << 20) - 1); continue; }
        u64 *pte = &tbl(e2)[(va >> 12) & 511];
        if (PTE_HAS(*pte)) fn(cr3, pte, va, arg);
        va += PAGE;
    }
}
static void unmap_fn(u64 cr3, u64 *pte, u64 va, int arg) {
    (void)arg;
    if (!(*pte & PTE_SOFT_SHARED)) pmm_free(*pte & PTE_ADDR);
    *pte = 0;
    flush(cr3, va);
}
static void protect_fn(u64 cr3, u64 *pte, u64 va, int prot) {
    u64 keep = *pte & (PTE_ADDR | PTE_SOFT_SHARED);
    *pte = (prot & AS_NONE) ? keep | PTE_SOFT_PARKED : (*pte & PTE_ADDR) | pte_bits(prot | ((*pte & PTE_SOFT_SHARED) ? AS_SHARED : 0));
    flush(cr3, va);
}
void as_unmap_range(u64 cr3, u64 start, u64 end) { as_walk(cr3, start, end, unmap_fn, 0); }
void as_protect_range(u64 cr3, u64 start, u64 end, int prot) { as_walk(cr3, start, end, protect_fn, prot); }

static void free_pdpt(u64 *pdpt) {
    for (int g = 0; g < 512; g++) {
        if (!(pdpt[g] & PTE_P) || !(pdpt[g] & PTE_U)) continue;
        u64 *pd = tbl(pdpt[g]);
        for (int i = 0; i < 512; i++) {
            if (!(pd[i] & PTE_P)) continue;
            u64 *pt = tbl(pd[i]);
            for (int j = 0; j < 512; j++) if (PTE_HAS(pt[j]) && !(pt[j] & PTE_SOFT_SHARED)) pmm_free(pt[j] & PTE_ADDR);
            pmm_free(pd[i] & PTE_ADDR);
        }
        pmm_free(pdpt[g] & PTE_ADDR);
    }
}

void as_destroy(u64 cr3) {
    u64 *pml4 = tbl(cr3);
    free_pdpt(tbl(pml4[0]));
    pmm_free(pml4[0] & PTE_ADDR);
    for (int i = PML4_USER_FIRST; i < PML4_USER_END; i++)
        if (pml4[i] & PTE_P) { free_pdpt(tbl(pml4[i])); pmm_free(pml4[i] & PTE_ADDR); }
    pmm_free(cr3);
}
