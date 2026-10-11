/* uarch.h - 64-bit ARM under the user-mode layer (src/user): threads, address spaces
 * and the trap frame with the names the x86-64 kernel gives them. */
#pragma once
#include "arm.h"
#include "sched.h"

typedef struct thr thread_t;

/* the exception frame boot.S builds: the FP/SIMD registers (q0-q31, FPSR, FPCR), then
 * x0-x30, the return address, the saved PSTATE and the program's stack pointer */
typedef struct { u8 fp[528] __attribute__((aligned(16))); u64 x[31]; u64 pc, pstate, sp; } frame_t;
#define FRAME_FP(f)   ((f)->fp)

#define PAGE 4096ull
/* programs live in [512 GB, 128 TB): top-level entries 1..255 (umm.c) */
#define USER_BASE      0x8000000000ull
#define USER_SIGTRAMP  (USER_BASE + PAGE)                 /* rt_sigreturn, mapped in every process */
#define USER_LOW_START (USER_BASE + 2 * PAGE)
#define USER_TOP       (USER_BASE + 0x40000000ull)       /* the low GiB: image, brk, stack */
#define USER_PIE_BASE_ (USER_BASE + 0x10000000ull)
#define USER_LOW_MMAP_ (USER_BASE + 0x30000000ull)
#define USER_HIGH_BASE (USER_BASE + 0x100000000ull)      /* mmap */
#define USER_HIGH_END  0x800000000000ull
#define USER_HOLE_BASE USER_HIGH_BASE                     /* no hole (x86-64 has one in its high region) */
#define USER_HOLE_END  USER_HIGH_BASE

/* page protections for as_map / as_protect (as x86-64's mm.h) */
#define AS_W      1
#define AS_X      2
#define AS_SHARED 4
#define AS_NONE   8
#define AS_UC     16
int   user_va(u64 va);
int   user_high_range(u64 start, u64 end);
u64   as_create(void);
u64   as_clone(u64 cr3);
void  as_destroy(u64 cr3);
int   as_map(u64 cr3, u64 va, u64 frame, int prot);
u64   as_translate(u64 cr3, u64 va);
int   as_pte_writable(u64 cr3, u64 va);
void  as_unmap(u64 cr3, u64 va);
void  as_protect(u64 cr3, u64 va, int prot);
int   as_parked(u64 cr3, u64 va);
void  as_unmap_range(u64 cr3, u64 start, u64 end);
void  as_protect_range(u64 cr3, u64 start, u64 end, int prot);
int   as_move(u64 cr3, u64 from, u64 to);
u64   kernel_cr3(void);
void  phys_write(u64 pa, const void *src, usize len);
u64   pmm_total_bytes(void);

/* threads: sched.c's, by x86-64's names */
int arm_helpers(void);                                  /* smp.c: cores besides this one that run programs */
#define ncpus (1 + arm_helpers())
static inline thread_t *thread_current(void) { return thr_self(); }
static inline void thread_yield(void) { thr_yield(); }
static inline void thread_sleep_ms(u64 ms) { thr_sleep_us((ms ? ms : 1) * 1000); }
static inline void thread_wake(thread_t *t) { if (t) thr_wake(t); }
static inline void thread_stop(thread_t *t) { thr_stop(t); }
static inline void thread_exit(void) { thr_exit(); }
static inline int  sched_threads(thread_t **out, int max) { return thr_list(out, max); }
int sched_smp_enabled(void);                            /* smp.c: programs run on the other cores too */
void arm_helpers_quiesce(u64 cr3);                  /* smp.c: no helper core has it loaded */
static inline void sched_quiesce_cr3(u64 cr3) { arm_helpers_quiesce(cr3); }
static inline void tlb_shootdown(u64 cr3) { (void)cr3; }
void      thread_block(void);
thread_t *thread_create_suspended(const char *name, void (*fn)(void *), void *arg, u64 cr3);
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3);
void      cpu_load_cr3(u64 cr3);
void      kernel_symbol(u64 addr, char *out, usize cap);

static inline void cli(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }

/* the trap frame, both architectures' way (src/user/uarch.h has x86-64's) */
#define FRAME_RET(f)       ((f)->x[0])
#define FRAME_SP(f)        ((f)->sp)
#define FRAME_PC(f)        ((f)->pc)
#define FRAME_SET_TLS(v)   (thread_current()->fs_base = (v), SYSREG_W(tpidr_el0, (v)))
void arm_user_frame(frame_t *f, u64 pc, u64 sp);  /* a fresh start at pc */
