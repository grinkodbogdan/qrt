/* cpu.h - x86-64 instructions, MSRs and control registers. */
#pragma once
#include "../../kernel/kernel.h"

static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline u8   inb(u16 p)         { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void)      { outb(0x80, 0); }

static inline u64 rdmsr(u32 m) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 m, u64 v) {
    __asm__ volatile("wrmsr" : : "c"(m), "a"((u32)v), "d"((u32)(v >> 32)));
}

static inline u64 read_cr0(void) { u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr4(void) { u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(u64 v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void write_cr3(u64 v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void write_cr4(u64 v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

static inline void cli(void)   { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void)   { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void)   { __asm__ volatile("hlt" ::: "memory"); }
static inline void pause(void) { __asm__ volatile("pause" ::: "memory"); }
static inline void sti_hlt(void) { __asm__ volatile("sti; hlt" ::: "memory"); }   /* atomic: no lost wakeup */
static inline u64 irq_save(void) { u64 f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_restore(u64 f) { if (f & 0x200) sti(); }
static inline u64 rdtsc64(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }

static inline void cpuid(u32 leaf, u32 sub, u32 r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

#define MSR_APIC_BASE    0x1b
#define MSR_PAT          0x277
#define MSR_EFER         0xc0000080
#define MSR_STAR         0xc0000081
#define MSR_LSTAR        0xc0000082
#define MSR_SFMASK       0xc0000084
#define MSR_FS_BASE      0xc0000100
#define MSR_GS_BASE      0xc0000101
#define MSR_KERNEL_GS    0xc0000102

/* segment selectors (see gdt.c) */
#define SEL_KCODE  0x08
#define SEL_KDATA  0x10
#define SEL_UDATA  0x20   /* | 3 */
#define SEL_UCODE  0x28   /* | 3 */
#define SEL_TSS    0x30

/* interrupt vectors */
#define VEC_TIMER    0x20
#define VEC_WAKE     0x40   /* IPI: render work available */
#define VEC_SPURIOUS 0xff
