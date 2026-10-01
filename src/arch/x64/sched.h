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
    u64 cpu_ticks;              /* ticks spent running */
    struct thread *next;
    u8 fpu[512] __attribute__((aligned(16)));
} thread_t;

void      sched_init(void);                         /* current context -> thread "shell" */
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3);
thread_t *thread_current(void);
void      thread_sleep_ms(u64 ms);
void      thread_yield(void);
void      thread_block(void);                       /* until thread_wake() */
void      thread_wake(thread_t *t);
void      thread_exit(void);                        /* never returns */
int       sched_threads(thread_t **out, int max);   /* snapshot for the System app */
u64       sched_idle_ticks(void);
