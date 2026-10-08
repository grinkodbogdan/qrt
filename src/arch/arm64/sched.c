/*
 * sched.c - threads on 64-bit ARM: cooperative, one core.  A thread runs until it
 * blocks (a semaphore, a mutex, a join), sleeps or yields; the shell yields every frame
 * (hal_wait_frame).  Linux (linux.c) runs on these threads: its kernel threads, its
 * timers, the interrupt poller and the input bridges.
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
static thr_t *cur = &main_thr, *ring = &main_thr;
static int nthreads = 1;

thr_t *thr_self(void) { return cur; }
int thr_count(void) { return nthreads; }

static void reap(void) {
    for (thr_t *t = ring; t; t = t->next == ring ? NULL : t->next)
        if (t->state == T_DONE && t->detached && !t->reaped && t != cur) {
            t->reaped = 1;
            kfree(t->stack);
            t->stack = NULL;
        }
}

/* the next thread to run: round robin from the current one; with none ready, wait for
 * the earliest sleeper (no interrupts: the CPU idles with yield) */
static void schedule(void) {
    for (;;) {
        u64 now = k_now_us(), soonest = ~0ull;
        thr_t *t = cur->next;
        for (int i = 0; i < nthreads; i++, t = t->next) {
            if (t->state == T_SLEEP) {
                if (t->wake_at <= now) t->state = T_READY;
                else if (t->wake_at < soonest) soonest = t->wake_at;
            }
            if (t->state == T_READY) {
                if (t != cur) {
                    thr_t *prev = cur;
                    cur = t;
                    arm_switch(&prev->sp, t->sp);
                    reap();
                }
                return;
            }
        }
        while (k_now_us() < soonest) __asm__ volatile("yield");
    }
}

void thr_yield(void) { schedule(); }
void thr_sleep_us(u64 us) { cur->wake_at = k_now_us() + us; cur->state = T_SLEEP; schedule(); }
void thr_sleep_until(u64 us) { if (us <= k_now_us()) { schedule(); return; } cur->wake_at = us; cur->state = T_SLEEP; schedule(); }
void thr_wake(thr_t *t) { if (t && (t->state == T_SLEEP || t->state == T_BLOCKED)) t->state = T_READY; }

void thr_entry(thr_t *t) {
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
    t->next = cur->next;
    cur->next = t;
    nthreads++;
    return t;
}

void thr_exit(void) {
    cur->state = T_DONE;
    if (cur->joiner) thr_wake(cur->joiner);
    schedule();
    for (;;) {}                                                   /* never runs again */
}
void thr_detach(thr_t *t) { t->detached = 1; }
int thr_join(thr_t *t) {
    while (t->state != T_DONE) { t->joiner = cur; cur->state = T_BLOCKED; schedule(); }
    t->detached = 1;
    return 0;
}

/* ---- semaphores and mutexes: a count and a FIFO of waiters ---- */
struct sem { int count; thr_t *head, *tail; };
static void wq_add(sem_t *s) {
    cur->wnext = NULL;
    if (s->tail) s->tail->wnext = cur; else s->head = cur;
    s->tail = cur;
}
sem_t *sem_new(int count) { sem_t *s = kalloc(sizeof *s); s->count = count; return s; }
void sem_del(sem_t *s) { kfree(s); }
void sem_down(sem_t *s) {
    while (s->count <= 0) { wq_add(s); cur->state = T_BLOCKED; schedule(); }
    s->count--;
}
void sem_up(sem_t *s) {
    s->count++;
    thr_t *t = s->head;
    if (t) { s->head = t->wnext; if (!s->head) s->tail = NULL; thr_wake(t); }
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
int  tls_key_new(void) { for (int i = 0; i < THR_TLS; i++) if (!tls_used[i]) { tls_used[i] = 1; return i; } return -1; }
void tls_key_del(int k) { if (k >= 0 && k < THR_TLS) tls_used[k] = 0; }
void tls_put(int k, void *v) { if (k >= 0 && k < THR_TLS) cur->tls[k] = v; }
void *tls_fetch(int k) { return k >= 0 && k < THR_TLS ? cur->tls[k] : NULL; }

int thr_is_main(void) { return cur == &main_thr; }
/* a thread that faulted: never runs again; the others go on */
void thr_park(void) { cur->state = T_BLOCKED; schedule(); for (;;) {} }
