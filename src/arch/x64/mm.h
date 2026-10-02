/* mm.h - physical memory, kernel heap and page tables (native mode). */
#pragma once
#include "cpu.h"

#define PAGE 4096ull
#define USER_TOP   0x40000000ull     /* user space: [0, 1 GiB) of every process ... */
#define USER_HIGH_BASE 0x1000000000ull   /* ... and [64 GiB, 128 TiB): above any RAM the identity map covers, */
#define USER_HOLE_BASE 0x8000000000ull   /* except [512 GiB, 1 TiB), where the kernel maps 64-bit device BARs */
#define USER_HOLE_END  0x10000000000ull
#define USER_HIGH_END  0x800000000000ull
#define HIGH_POOL  0x40000000ull     /* kernel memory always comes from >= 1 GiB */

typedef struct {
    EFI_MEMORY_DESCRIPTOR *map;
    usize map_size, desc_size;
    u64 fb_base, fb_size;
} mm_boot_t;

void  mm_init(const mm_boot_t *b, const u64 *reserve, int nreserve);  /* pmm + kernel page tables */
u64   pmm_alloc(int high);             /* one zeroed 4 KiB frame; high = from the >= 1 GiB pool */
u64   pmm_alloc_contig(usize pages);   /* high pool, zeroed */
void  pmm_free(u64 frame);
u64   pmm_free_bytes(void);
u64   pmm_total_bytes(void);

void *heap_alloc(usize n);
void  heap_free(void *p);
int   heap_owns(const void *p);

u64   kernel_cr3(void);
void  phys_write(u64 pa, const void *src, usize len);  /* works for frames under the user window */
u64   mm_max_phys(void);
void  mm_uncached(u64 base, u64 size);              /* device registers: uncached in the identity map */
void *mm_map_mmio(u64 base, u64 size);             /* device registers anywhere (high 64-bit BARs too) */

/* user address spaces */
u64   as_create(void);                                   /* returns CR3 */
u64   as_clone(u64 cr3);                                 /* fork: a copy of the user pages */
void  as_destroy(u64 cr3);
/* page protections for as_map / as_protect */
#define AS_W      1                  /* writable */
#define AS_X      2                  /* executable (without it: no-execute, where the CPU has NX) */
#define AS_SHARED 4                  /* the frame belongs to a shared object: never freed with the mapping */
#define AS_NONE   8                  /* as_protect: no access, the page and its contents are kept */
int   user_va(u64 va);                                   /* in one of the user regions */
int   user_high_range(u64 start, u64 end);               /* [start, end) inside one high user region */
int   as_map(u64 cr3, u64 va, u64 frame, int prot);      /* 4 KiB, user-accessible */
u64   as_translate(u64 cr3, u64 va);                     /* physical address or 0 */
int   as_pte_writable(u64 cr3, u64 va);                  /* present and writable */
void  as_unmap(u64 cr3, u64 va);                         /* frees the frame unless AS_SHARED */
void  as_protect(u64 cr3, u64 va, int prot);             /* change a page's protection (AS_NONE parks it) */
int   as_parked(u64 cr3, u64 va);
void  as_unmap_range(u64 cr3, u64 start, u64 end);
void  as_protect_range(u64 cr3, u64 start, u64 end, int prot);   /* the shared bit of each page is kept */                        /* a page parked by AS_NONE */
int   as_move(u64 cr3, u64 from, u64 to);                /* mremap: move a page's frame; 1 if one was there */
void  mm_enable_nx(void);
