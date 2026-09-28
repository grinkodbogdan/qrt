/* mm.h - physical memory, kernel heap and page tables (native mode). */
#pragma once
#include "cpu.h"

#define PAGE 4096ull
#define USER_TOP   0x40000000ull     /* user space: [0, 1 GiB) of every process */
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

/* user address spaces */
u64   as_create(void);                                   /* returns CR3 */
void  as_destroy(u64 cr3);
int   as_map(u64 cr3, u64 va, u64 frame, int writable);  /* 4 KiB, user-accessible */
u64   as_translate(u64 cr3, u64 va);                     /* physical address or 0 */
void  as_unmap(u64 cr3, u64 va);
