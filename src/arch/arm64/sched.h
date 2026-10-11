/* sched.h - cooperative threads on 64-bit ARM (sched.c) */
#pragma once
#include "../../kernel/kernel.h"

#define THR_TLS 32
typedef struct thr thr_t;
enum { T_RUNNABLE, T_SLEEPING, T_BLOCKED, T_DEAD };
struct proc;
struct thr {
    u64 sp;
    u8 *stack;
    int state, detached, reaped;
    u64 wake_at;                     /* T_SLEEPING: k_now_us() to wake at */
    void (*fn)(void *);
    void *arg;
    void *tls[THR_TLS];
    const char *name;
    struct thr *next;                /* every thread, in a ring */
    struct thr *wnext;               /* a wait queue */
    struct thr *joiner;
    u64 run_us;                      /* CPU time since the last thr_report */
    int bkl_saved;                   /* the big lock's depth, given up while it waits */
    int bkl_waiting;
    /* a program's thread (src/user, through uarch.h): the fields x86-64's thread_t has */
    u64 cr3;                         /* TTBR0 (table | ASID << 48); 0: the kernel's */
    u64 fs_base;                     /* TPIDR_EL0: the program's thread pointer */
    u64 kstack, kstack_top;
    struct proc *proc;               /* NULL for kernel threads */
    int tid;
    u64 clear_tid;
    volatile int in_sys;
    u32 sys_nr; u64 sys_a0;
    int kbuf;
    int irq_depth;
    volatile int on_cpu;
    u64 cpu_ticks;
    int prio;
    u64 sig_mask, sig_pending, sig_saved_mask;
    int sig_suspended;
    u64 alt_sp, alt_size;
    int alt_flags;
    u64 fault_esr, fault_far;        /* the exception being handled (signal.c) */
    u64 wp_page[16];                 /* read-only pages this system call writes (uentry.c): 8 runs [start, end) */
    int wp_n;
    void *uw;                        /* the wait it is in (src/user/uwait.c) */
    struct urun *urun;               /* its runs on the helper cores (smp.c) */
};
typedef struct sem sem_t;
typedef struct mtx mtx_t;

thr_t *thr_create(const char *name, void (*fn)(void *), void *arg, usize stack);
thr_t *thr_self(void);
const char *thr_name(thr_t *t);
int   thr_count(void);
void  thr_yield(void);
void  thr_sleep_us(u64 us);
void  thr_sleep_until(u64 us);       /* k_now_us() deadline */
void  thr_wake(thr_t *t);
void  thr_exit(void);
void  thr_detach(thr_t *t);
int   thr_join(thr_t *t);

sem_t *sem_new(int count);
void  sem_del(sem_t *s);
void  sem_down(sem_t *s);
void  sem_up(sem_t *s);
mtx_t *mtx_new(int recursive);
void  mtx_del(mtx_t *m);
void  mtx_lock(mtx_t *m);
void  mtx_unlock(mtx_t *m);

int   tls_key_new(void);
void  tls_key_del(int k);
void  tls_put(int k, void *v);
void *tls_fetch(int k);
int   thr_is_main(void);
void  thr_park(void);
void  thr_tick(void);                /* the timer interrupt (gic.c) */
u64   thr_switches(void);
thr_t *thr_create_suspended(const char *name, void (*fn)(void *), void *arg, usize stack);   /* runs after thr_wake() */
void  thr_block(void);               /* until thr_wake() */
thr_t *bkl_owner_thr(void);          /* the big lock's holder (the watchdog's report) */
void  thr_stop(thr_t *t);            /* never runs again (a stopped program's thread) */
int   thr_list(thr_t **out, int max);/* the live threads */
void  bkl_lock(void);                /* the big lock of the user-mode layer: kept across preemption, */
void  bkl_unlock(void);              /* given up while the holder blocks or sleeps */
int   bkl_held(void);
void  thr_report(void);             /* klog: where the CPU went since the last call */
