/*
 * sched.c - threads on 64-bit ARM, one core, preemptive: a thread runs until it blocks
 * (a semaphore, a mutex, a join), sleeps or yields - or until the 10 ms timer tick
 * (gic.c) finds another thread ready.  Linux (linux.c) runs on these threads: its kernel
 * threads, its timers, the interrupt poller and the input bridges; a driver that spins
 * on a register no longer stops the shell.
 *
 * Every operation on the scheduler's state runs with interrupts masked.  Each thread
 * keeps its own interrupt mask across a switch (sw() saves and restores DAIF), so a
 * thread preempted inside the timer interrupt resumes there and returns from it.
 */
#include "arm.h"
#include "sched.h"

enum { T_READY, T_SLEEP, T_BLOCKED, T_DONE };
struct thr {
    u64 sp;
    u8 *stack;
    int state, detached, reaped;
    u64 wake_at;                     /* T_SLEEP: k_now_us() to wake at */
    void (*fn)(void *);
    void *arg;
    void *tls[THR_TLS];
    const char *name;
    struct thr *next;                /* every thread, in a ring */
    struct thr *wnext;               /* a wait queue */
    struct thr *joiner;
};

void arm_switch(u64 *save_sp, u64 new_sp);
void arm_thread_start(void);

static thr_t main_thr = { .state = T_READY, .name = "shell", .next = &main_thr };
static thr_t *cur = &main_thr;
static int nthreads = 1;
static u64 switches;

thr_t *thr_self(void) { return cur; }
int thr_count(void) { return nthreads; }
u64 thr_switches(void) { return switches; }
int thr_is_main(void) { return cur == &main_thr; }

static void reap(void) {
    thr_t *t = &main_thr;
    do {
        if (t->state == T_DONE && t->detached && !t->reaped && t != cur) {
            t->reaped = 1;
            kfree(t->stack);
            t->stack = NULL;
        }
        t = t->next;
    } while (t != &main_thr);
}

/* switch to t; we come back here when something switches to us again */
static void sw(thr_t *t) {
    thr_t *prev = cur;
    u64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    cur = t;
    switches++;
    arm_switch(&prev->sp, t->sp);
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
    reap();
}

/* the next ready thread after the current one, waking sleepers that are due */
static thr_t *pick(u64 *soonest) {
    u64 now = k_now_us();
    *soonest = ~0ull;
    thr_t *t = cur->next;
    for (int i = 0; i < nthreads; i++, t = t->next) {
        if (t->state == T_SLEEP) {
            if (t->wake_at <= now) t->state = T_READY;
            else if (t->wake_at < *soonest) *soonest = t->wake_at;
        }
        if (t->state == T_READY) return t;
    }
    return NULL;
}

/* interrupts masked: run the next ready thread (maybe the current one); with none, idle
 * until the earliest sleeper is due */
static void schedule(void) {
    for (;;) {
        u64 soonest;
        thr_t *t = pick(&soonest);
        if (t) { if (t != cur) sw(t); return; }
        while (k_now_us() < soonest) __asm__ volatile("wfi");     /* the 10 ms tick wakes it */
    }
}

/* the timer tick (gic.c, interrupts masked): another ready thread gets the CPU */
void thr_tick(void) {
    u64 soonest;
    thr_t *t = pick(&soonest);
    if (t && t != cur && cur->state == T_READY) sw(t);
}

void thr_yield(void) { u64 f = irq_save(); schedule(); irq_restore(f); }
void thr_sleep_us(u64 us) {
    u64 f = irq_save();
    cur->wake_at = k_now_us() + us;
    cur->state = T_SLEEP;
    schedule();
    irq_restore(f);
}
void thr_sleep_until(u64 us) {
    u64 f = irq_save();
    if (us > k_now_us()) { cur->wake_at = us; cur->state = T_SLEEP; }
    schedule();
    irq_restore(f);
}
void thr_wake(thr_t *t) {
    u64 f = irq_save();
    if (t && (t->state == T_SLEEP || t->state == T_BLOCKED)) t->state = T_READY;
    irq_restore(f);
}

void thr_entry(thr_t *t) {
    __asm__ volatile("msr daifclr, #2");                          /* a new thread can be preempted */
    t->fn(t->arg);
    thr_exit();
}

thr_t *thr_create(const char *name, void (*fn)(void *), void *arg, usize stack) {
    thr_t *t = kalloc(sizeof *t);
    t->stack = kalloc(stack);
    t->fn = fn; t->arg = arg; t->name = name;
    u64 *sp = (u64 *)((usize)(t->stack + stack) & ~15ull) - 20;  /* arm_switch's frame: 160 bytes */
    memset(sp, 0, 160);
    sp[0] = (u64)(usize)t;                                        /* x19 */
    sp[11] = (u64)(usize)arm_thread_start;                        /* x30 */
    t->sp = (u64)(usize)sp;
    t->state = T_READY;
    u64 f = irq_save();
    t->next = cur->next;
    cur->next = t;
    nthreads++;
    irq_restore(f);
    return t;
}

void thr_exit(void) {
    irq_save();
    cur->state = T_DONE;
    if (cur->joiner && cur->joiner->state == T_BLOCKED) cur->joiner->state = T_READY;
    schedule();
    for (;;) {}                                                   /* never runs again */
}
void thr_detach(thr_t *t) { t->detached = 1; }
int thr_join(thr_t *t) {
    u64 f = irq_save();
    while (t->state != T_DONE) { t->joiner = cur; cur->state = T_BLOCKED; schedule(); }
    t->detached = 1;
    irq_restore(f);
    return 0;
}
/* a thread that faulted: never runs again; the others go on */
void thr_park(void) { irq_save(); cur->state = T_BLOCKED; schedule(); for (;;) {} }

/* ---- semaphores and mutexes: a count and a FIFO of waiters ---- */
struct sem { int count; thr_t *head, *tail; };
sem_t *sem_new(int count) { sem_t *s = kalloc(sizeof *s); s->count = count; return s; }
void sem_del(sem_t *s) { kfree(s); }
void sem_down(sem_t *s) {
    u64 f = irq_save();
    while (s->count <= 0) {
        cur->wnext = NULL;
        if (s->tail) s->tail->wnext = cur; else s->head = cur;
        s->tail = cur;
        cur->state = T_BLOCKED;
        schedule();
    }
    s->count--;
    irq_restore(f);
}
void sem_up(sem_t *s) {
    u64 f = irq_save();
    s->count++;
    thr_t *t = s->head;
    if (t) { s->head = t->wnext; if (!s->head) s->tail = NULL; if (t->state == T_BLOCKED) t->state = T_READY; }
    irq_restore(f);
}

struct mtx { sem_t sem; thr_t *owner; int depth, recursive; };
mtx_t *mtx_new(int recursive) { mtx_t *m = kalloc(sizeof *m); m->sem.count = 1; m->recursive = recursive; return m; }
void mtx_del(mtx_t *m) { kfree(m); }
void mtx_lock(mtx_t *m) {
    if (m->recursive && m->owner == cur) { m->depth++; return; }
    sem_down(&m->sem);
    m->owner = cur;
    m->depth = 1;
}
void mtx_unlock(mtx_t *m) {
    if (--m->depth > 0) return;
    m->owner = NULL;
    sem_up(&m->sem);
}

/* ---- thread-local slots ---- */
static int tls_used[THR_TLS];
int  tls_key_new(void) {
    u64 f = irq_save();
    int k = -1;
    for (int i = 0; i < THR_TLS && k < 0; i++) if (!tls_used[i]) { tls_used[i] = 1; k = i; }
    irq_restore(f);
    return k;
}
void tls_key_del(int k) { if (k >= 0 && k < THR_TLS) tls_used[k] = 0; }
void tls_put(int k, void *v) { if (k >= 0 && k < THR_TLS) cur->tls[k] = v; }
void *tls_fetch(int k) { return k >= 0 && k < THR_TLS ? cur->tls[k] : NULL; }
