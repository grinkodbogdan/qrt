/* sched.h - threads and the scheduler (boot core). */
#pragma once
#include "arch.h"

typedef enum { T_RUNNABLE, T_SLEEPING, T_BLOCKED, T_DEAD } tstate_t;

struct proc;

typedef struct thread {
    u64 rsp;                    /* saved kernel stack pointer (switch_context) */
    u64 kstack, kstack_top;
    u64 cr3;                    /* address space: kernel_cr3() for kernel threads */
    u64 fs_base;                /* user TLS pointer */
    tstate_t state;
    u64 wake_tick;
    const char *name;
    struct proc *proc;          /* NULL for kernel threads */
    int tid;                    /* Linux thread id (user threads) */
    u64 clear_tid;              /* CLONE_CHILD_CLEARTID / set_tid_address: zeroed and woken at exit */
    volatile int in_sys;        /* inside a system call: may hold kernel locks */
    int kbuf;                   /* sendfile: the kernel's own buffer stands in for a user one */
    int irq_depth;              /* inside interrupt handlers: a fault there is the kernel's, not the program's */
    volatile int on_cpu;        /* the core running it, -1 if none */
    u64 cpu_ticks;              /* ticks spent running */
    u64 sig_mask, sig_pending;  /* blocked signals; signals sent to this thread */
    u64 sig_saved_mask;         /* rt_sigsuspend: the mask to restore after the handler */
    int sig_suspended;
    u64 alt_sp, alt_size;       /* sigaltstack */
    int alt_flags;
    struct thread *next;
    u8 fpu[512] __attribute__((aligned(16)));
} thread_t;

void      sched_init(void);                         /* current context -> thread "shell" */
thread_t *thread_create_suspended(const char *name, void (*fn)(void *), void *arg, u64 cr3);   /* runs after thread_wake() */
void      thread_stop(thread_t *t);                 /* a thread of a stopped process: never runs again (any core) */
/* SMP (0.9.5): program threads on every core, the kernel under one big lock */
void      bkl_lock(void);
void      bkl_unlock(void);
int       bkl_held(void);
void      tlb_shootdown(u64 cr3);                   /* other cores using cr3 reload it */
void      cpu_load_cr3(u64 cr3);                    /* switch this core's address space */
void      sched_quiesce_cr3(u64 cr3);               /* wait until no other core uses cr3 */
void      sched_smp_enable(void);
int       sched_smp_enabled(void);
void      sched_ap_join(void (*idle_fn)(void *));   /* a worker core starts scheduling; never returns */
int       sched_ap_has_work(void);
void      sched_ap_run(void);
void      sched_tlb_ipi(void);
void      thread_first_run(void);
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3);
thread_t *thread_current(void);
void      thread_sleep_ms(u64 ms);
void      thread_yield(void);
void      thread_block(void);                       /* until thread_wake() */
void      thread_wake(thread_t *t);
void      kernel_symbol(u64 addr, char *out, usize cap);   /* native.c: "function+offset" for a kernel address */
void      thread_exit(void);                        /* never returns */
int       sched_threads(thread_t **out, int max);   /* snapshot for the System app */
u64       sched_idle_ticks(void);
