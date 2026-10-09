/* sched.h - cooperative threads on 64-bit ARM (sched.c) */
#pragma once
#include "../../kernel/kernel.h"

#define THR_TLS 32
typedef struct thr thr_t;
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
void  thr_report(void);             /* klog: where the CPU went since the last call */
