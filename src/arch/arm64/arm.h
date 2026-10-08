/* arm.h - Tessera on 64-bit ARM: the pieces the shared kernel code calls under k.native. */
#pragma once
#include "../../kernel/kernel.h"
#include "../../drivers/uart.h"

#define R32(a)      (*(volatile u32 *)(usize)(a))
#define W32(a, v)   (*(volatile u32 *)(usize)(a) = (u32)(v))
#define SYSREG_R(r) ({ u64 _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define SYSREG_W(r, v) __asm__ volatile("msr " #r ", %0" : : "r"((u64)(v)))

/* main.c */
void *heap_alloc(usize n);
void  heap_free(void *p);
int   heap_owns(const void *p);
u64   irq_save(void);
void  irq_restore(u64 flags);
void  native_panic(const char *what, void *frame);
extern u64 arm_ram_base, arm_ram_size;
int   arm_is_ram(u64 addr, u64 len);              /* inside a /memory bank */      /* the largest RAM bank */
extern const char *arm_model;

/* mmu.c: identity map, RAM cacheable, the rest device memory; fb uncached */
void  mmu_init(const u64 (*ram)[2], int nram, u64 fb_base, u64 fb_size, const u64 (*hole)[2], int nhole);
void  pmm_init(u64 base, u64 end);
u64   pmm_alloc_contig(usize pages);
u64   pmm_alloc(int high);
void  pmm_free(u64 frame);
u64   pmm_free_bytes(void);

/* fdt.c: the flattened device tree the boot loader passed */
int   fdt_init(const void *blob);
int   fdt_node(const char *path);                       /* offset or < 0 */
int   fdt_find_compatible(int from, const char *compat); /* next node after from (-1: first) */
const void *fdt_prop(int node, const char *name, int *len);
int   fdt_reg(int node, int i, u64 *addr, u64 *size);    /* the i-th reg entry (root's cell sizes) */
const char *fdt_name(int node);
int   fdt_children(int node, int (*fn)(int child, void *arg), void *arg);
u32   fdt_u32(const void *p);
u64   fdt_u64(const void *p);
usize fdt_size(void);

/* fb.c: QEMU's ramfb, or the display the boot loader left on (Qualcomm MDP5) */
int   fb_init(void);
void  fb_present(const u32 *px, int stride, int x, int y, int w, int h);
extern char fb_what[64];

/* virtio.c: QEMU's virtio-mmio keyboard and tablet */
void  virtio_input_init(void);
int   virtio_input_poll(event_t *out, int max);

/* msm.c: Qualcomm MSM8953 (Xiaomi Mi A1): volume key GPIO */
int   msm_init(void);
int   msm_poll(event_t *out, int max);
