/* arch.h - native x86-64 kernel: per-CPU state, interrupts, APIC, boot. */
#pragma once
#include "mm.h"

typedef struct __attribute__((packed)) {
    u32 rsv0;
    u64 rsp0, rsp1, rsp2;
    u64 rsv1;
    u64 ist[7];
    u64 rsv2;
    u16 rsv3, iomap;
} tss_t;

struct thread;

typedef struct percpu {
    struct percpu *self;        /* gs:0  */
    u64 kernel_rsp;             /* gs:8  stack for syscall entry */
    u64 user_rsp;               /* gs:16 scratch for syscall entry */
    u32 index, apic_id;         /* gs:24 */
    struct thread *cur;         /* gs:32 */
    u64 gdt[8];
    tss_t tss;
    u8 df_stack[8192] __attribute__((aligned(16)));
} percpu_t;

typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, err, rip, cs, rflags, rsp, ss;
} frame_t;

#define MAX_CPUS 16
extern percpu_t *cpus[MAX_CPUS];
extern int ncpus;

void cpu_setup(percpu_t *c, u32 index);  /* GDT, TSS, IDT, GS base, PAT, SSE for this core */
percpu_t *this_cpu(void);
void idt_init(void);

/* interrupts */
typedef void (*irq_handler_t)(frame_t *f);
void irq_register(int vector, irq_handler_t h);
void native_panic(const char *what, frame_t *f);    /* crash screen; never returns */
extern int (*user_fault_hook)(frame_t *f);          /* 1 = handled (process killed) */

/* local APIC */
void lapic_init(void);
void lapic_eoi(void);
u32  lapic_id(void);
void lapic_timer_start(u32 hz);
void lapic_send_ipi(u32 apic_id, u32 vector);
void lapic_broadcast_ipi(u32 vector);             /* all cores except this one */
void lapic_init_sipi(u32 apic_id, u32 page);
extern volatile u64 ticks;                        /* timer interrupts since start */
extern u32 tick_hz;

void k_delay_us(u64 us);
