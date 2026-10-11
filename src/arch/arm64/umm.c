/*
 * umm.c - address spaces for programs on 64-bit ARM: the same interface as the x86-64
 * kernel's (as_create, as_map, as_translate, ... in src/arch/x64/mm.c), for the shared
 * user-mode layer in src/user.
 *
 * 48-bit virtual addresses, four levels of tables.  Every process has its own top-level
 * table: its entry 0 - the first 512 GB, the identity map and Argon's window - is the
 * kernel's (the same table, shared), so the kernel runs on any process's tables; entries
 * 1..255 - virtual 512 GB to 128 TB, as much as Linux gives an AArch64 program - are the
 * program's, built here page by page: accessible from EL0, never executable by the
 * kernel (PXN), not global (nG: tagged with the process's ASID, so switching between
 * programs needs no TLB flush).
 */
#include "arm.h"
#include "uarch.h"

#define D_PAGE   3ull
#define D_TABLE  3ull
#define D_EL0    (1ull << 6)          /* AP[1]: EL0 may access */
#define D_RO     (1ull << 7)          /* AP[2]: read-only (EL0 and EL1) */
#define D_ISH    (3ull << 8)
#define D_AF     (1ull << 10)
#define D_NG     (1ull << 11)
#define D_PXN    (1ull << 53)
#define D_UXN    (1ull << 54)
#define D_SHARED (1ull << 55)         /* software: the frame belongs to a shared object */
#define D_PARKED (1ull << 56)         /* software: PROT_NONE, the page kept */
#define D_ADDR   0x0000fffffffff000ull
#define HAS(e)   (((e) & 1) || ((e) & D_PARKED))
#define ATTR_NORMAL (1ull << 2)       /* MAIR index 1 (mmu.c) */
#define ATTR_DEVICE (0ull << 2)

#define L0_USER 1                     /* top-level entries 1..255 */
#define L0_END  256
u64 *mmu_kernel_l0(void);

static u64 *tbl(u64 e) { return (u64 *)(usize)(e & D_ADDR); }
static u64 asid_of(u64 cr3) { return cr3 >> 48; }

int user_va(u64 va) { return va >= USER_BASE + PAGE && va < USER_HIGH_END; }
int user_high_range(u64 start, u64 end) { return start >= USER_HIGH_BASE && end <= USER_HIGH_END && start < end; }

/* ---- ASIDs: 1..255, one per live address space; 0 when they run out (flushed on every switch) */
static u8 asid_used[256];
static u64 asid_take(void) {
    for (int i = 1; i < 256; i++) if (!asid_used[i]) { asid_used[i] = 1; return (u64)i; }
    return 0;
}

static void tlb_asid(u64 cr3) {
    __asm__ volatile("dsb ishst; tlbi aside1is, %0; dsb ish; isb" : : "r"(asid_of(cr3) << 48) : "memory");
}
static void tlb_page(u64 cr3, u64 va) {
    __asm__ volatile("dsb ishst; tlbi vae1is, %0; dsb ish" : : "r"((va >> 12) | (asid_of(cr3) << 48)) : "memory");
}
/* code written through the kernel's (data) view of a page: make the instruction side see it */
static void sync_icache(u64 pa) {
    for (u64 a = pa; a < pa + PAGE; a += 64) __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
}

u64 kernel_cr3(void) { return (u64)(usize)mmu_kernel_l0(); }

u64 as_create(void) {
    u64 *l0 = (u64 *)(usize)pmm_alloc(1);
    if (!l0) return 0;
    l0[0] = mmu_kernel_l0()[0];                                   /* the kernel's 512 GB */
    u64 cr3 = (u64)(usize)l0 | (asid_take() << 48);
    u64 as_sigtramp_frame(void);
    as_map(cr3, USER_SIGTRAMP, as_sigtramp_frame(), AS_X | AS_SHARED);
    return cr3;
}

static u64 *pte_of(u64 cr3, u64 va, int create) {
    if (!user_va(va) || cr3 == kernel_cr3()) return NULL;
    u64 *e = &tbl(cr3)[(va >> 39) & 511];
    for (int shift = 30; shift >= 12; shift -= 9) {
        if (!(*e & 1)) {
            if (!create) return NULL;
            u64 t = pmm_alloc(1);
            if (!t) return NULL;
            __asm__ volatile("dsb ishst" ::: "memory");
            *e = t | D_TABLE;
        }
        e = &tbl(*e)[(va >> shift) & 511];
    }
    return e;
}

static u64 bits(int prot) {
    u64 d = D_PAGE | D_AF | D_ISH | D_NG | D_EL0 | D_PXN;
    d |= (prot & AS_UC) ? ATTR_DEVICE | D_UXN : ATTR_NORMAL;
    if (!(prot & AS_W)) d |= D_RO;
    if (!(prot & AS_X)) d |= D_UXN;
    if (prot & AS_SHARED) d |= D_SHARED;
    return d;
}

/* write an entry: an old valid one goes first (break before make) */
static void set_pte(u64 cr3, u64 *pte, u64 va, u64 e) {
    if (*pte & 1) { *pte = 0; tlb_page(cr3, va); }
    if ((e & 1) && !(e & D_UXN)) sync_icache(e & D_ADDR);
    *pte = e;
    __asm__ volatile("dsb ishst; isb" ::: "memory");
}

int as_map(u64 cr3, u64 va, u64 frame, int prot) {
    u64 *pte = pte_of(cr3, va, 1);
    if (!pte) return -1;
    if (HAS(*pte) && !(*pte & D_SHARED) && (*pte & D_ADDR) != (frame & D_ADDR)) pmm_free(*pte & D_ADDR);
    set_pte(cr3, pte, va, (frame & D_ADDR) | bits(prot));
    return 0;
}

u64 as_translate(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    return pte && (*pte & 1) ? (*pte & D_ADDR) | (va & 0xfff) : 0;
}

int as_pte_writable(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    return pte && (*pte & 1) && !(*pte & D_RO);
}

void as_unmap(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    if (!pte || !HAS(*pte)) return;
    u64 e = *pte;
    set_pte(cr3, pte, va, 0);
    if (!(e & D_SHARED)) pmm_free(e & D_ADDR);
}

static u64 reprotect(u64 e, int prot) {
    u64 keep = e & (D_ADDR | D_SHARED);
    if (prot & AS_NONE) return keep | D_PARKED;
    return (e & D_ADDR) | bits(prot | ((e & D_SHARED) ? AS_SHARED : 0));
}

void as_protect(u64 cr3, u64 va, int prot) {
    u64 *pte = pte_of(cr3, va, 0);
    if (!pte || !HAS(*pte)) return;
    set_pte(cr3, pte, va, reprotect(*pte, prot));
}

int as_parked(u64 cr3, u64 va) { u64 *pte = pte_of(cr3, va, 0); return pte && (*pte & D_PARKED) && !(*pte & 1); }

int as_move(u64 cr3, u64 from, u64 to) {
    u64 *ps = pte_of(cr3, from, 0);
    if (!ps || !HAS(*ps)) return 0;
    u64 e = *ps;
    set_pte(cr3, ps, from, 0);
    u64 *pd = pte_of(cr3, to, 1);
    if (!pd) { if (!(e & D_SHARED)) pmm_free(e & D_ADDR); return 0; }
    set_pte(cr3, pd, to, e);
    return 1;
}

/* the present (or parked) pages of [start, end), skipping empty tables in bulk.  The TLB
 * is flushed once for the address space, not page by page (each flush goes to every
 * core: munmap and mprotect of a big range - Ladybird's heaps - kept the boot core busy
 * with them, 0.32.0); a permission change needs no break before make; freed frames go
 * back only after the flush, so no core can still reach them through its TLB. */
static void walk(u64 cr3, u64 start, u64 end, int prot, int unmap) {
    u64 va = MAX(start & ~(PAGE - 1), USER_BASE);
    end = MIN(end, USER_HIGH_END);
    u64 freeq[64];
    int nfree = 0, changed = 0, icache = 0;
    while (va < end) {
        u64 e0 = tbl(cr3)[(va >> 39) & 511];
        if (!(e0 & 1)) { va = (va + (1ull << 39)) & ~((1ull << 39) - 1); continue; }
        u64 e1 = tbl(e0)[(va >> 30) & 511];
        if (!(e1 & 1)) { va = (va + (1ull << 30)) & ~((1ull << 30) - 1); continue; }
        u64 e2 = tbl(e1)[(va >> 21) & 511];
        if (!(e2 & 1)) { va = (va + (2ull << 20)) & ~((2ull << 20) - 1); continue; }
        u64 *pte = &tbl(e2)[(va >> 12) & 511];
        if (HAS(*pte)) {
            u64 e = *pte, n = unmap ? 0 : reprotect(e, prot);
            if (n != e) {
                if ((n & 1) && !(n & D_UXN) && (!(e & 1) || (e & D_UXN))) {   /* becomes executable: clean its lines */
                    for (u64 a = n & D_ADDR; a < (n & D_ADDR) + PAGE; a += 64) __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
                    icache = 1;
                }
                *pte = n;
                changed = 1;
                if (unmap && !(e & D_SHARED)) {
                    if (nfree == 64) {
                        tlb_asid(cr3);
                        for (int i = 0; i < nfree; i++) pmm_free(freeq[i]);
                        nfree = 0;
                    }
                    freeq[nfree++] = e & D_ADDR;
                }
            }
        }
        va += PAGE;
    }
    if (changed) tlb_asid(cr3);
    if (icache) __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
    for (int i = 0; i < nfree; i++) pmm_free(freeq[i]);
}
void as_unmap_range(u64 cr3, u64 start, u64 end) { walk(cr3, start, end, 0, 1); }
void as_protect_range(u64 cr3, u64 start, u64 end, int prot) { walk(cr3, start, end, prot, 0); }

/* every page of the program's half: copied into dst (fork), counted, or freed with its tables */
enum { V_CLONE, V_COUNT, V_FREE };
static u64 visit(u64 cr3, int what, u64 dst) {
    u64 n = 0;
    for (int g = L0_USER; g < L0_END; g++) {
        u64 e0 = tbl(cr3)[g];
        if (!(e0 & 1)) continue;
        for (int h = 0; h < 512; h++) {
            u64 e1 = tbl(e0)[h];
            if (!(e1 & 1)) continue;
            for (int i = 0; i < 512; i++) {
                u64 e2 = tbl(e1)[i];
                if (!(e2 & 1)) continue;
                for (int j = 0; j < 512; j++) {
                    u64 e = tbl(e2)[j];
                    if (!HAS(e)) continue;
                    if (what == V_COUNT) { if (!(e & D_SHARED)) n++; continue; }
                    if (what == V_FREE) { if (!(e & D_SHARED)) pmm_free(e & D_ADDR); continue; }
                    u64 va = ((u64)g << 39) | ((u64)h << 30) | ((u64)i << 21) | ((u64)j << 12);
                    u64 *pd = pte_of(dst, va, 1);
                    if (!pd) continue;
                    if (e & D_SHARED) { *pd = e; continue; }
                    u64 f = pmm_alloc(0);
                    if (!f) continue;
                    memcpy((void *)(usize)f, (void *)(usize)(e & D_ADDR), PAGE);
                    *pd = f | (e & ~D_ADDR);
                }
                if (what == V_FREE) pmm_free(e2 & D_ADDR);
            }
            if (what == V_FREE) pmm_free(e1 & D_ADDR);
        }
        if (what == V_FREE) pmm_free(e0 & D_ADDR);
    }
    return n;
}

/* fork: a copy of every private page; shared ones stay shared */
u64 as_clone(u64 src) {
    u64 dst = as_create();
    visit(src, V_CLONE, dst);
    __asm__ volatile("dsb ishst" ::: "memory");
    return dst;
}

u64 as_private_pages(u64 cr3) { return visit(cr3, V_COUNT, 0); }

void as_destroy(u64 cr3) {
    visit(cr3, V_FREE, 0);
    tlb_asid(cr3);
    if (asid_of(cr3)) asid_used[asid_of(cr3)] = 0;
    pmm_free(cr3 & D_ADDR);
}

/* the page a program's signal handlers return through (Linux's vDSO sigreturn) */
static u64 tramp_frame;
u64 as_sigtramp_frame(void) {
    if (!tramp_frame) {
        tramp_frame = pmm_alloc(1);
        u32 *c = (u32 *)(usize)tramp_frame;
        c[0] = 0xd2801168;            /* mov x8, #139 (rt_sigreturn) */
        c[1] = 0xd4000001;            /* svc #0 */
    }
    return tramp_frame;
}

void phys_write(u64 pa, const void *src, usize len) { memcpy((void *)(usize)pa, src, len); }
u64 pmm_total_bytes(void) { return arm_ram_size; }
volatile int pmm_low;                         /* memory runs low (proc_oom_check) */

/* The x86-64 kernel writes into programs' read-only pages when a system call has to
 * (CR0.WP off: a file mapped read-only, filled by mmap).  ARM has no "the kernel may
 * write, the program may only read": the page is made writable for the rest of the
 * call and read-only again at its end (uentry.c). */
int as_open_write(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    if (!pte || !(*pte & 1) || !(*pte & D_RO)) return 0;
    set_pte(cr3, pte, va, *pte & ~D_RO);
    return 1;
}
void as_close_write(u64 cr3, u64 va) {
    u64 *pte = pte_of(cr3, va, 0);
    if (pte && (*pte & 1)) set_pte(cr3, pte, va, *pte | D_RO);
}
