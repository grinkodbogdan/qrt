/*
 * uwait.c - waiting for something to happen, for the Linux layer.
 *
 * A wait (poll, select, epoll_wait, a blocking read of a pipe or socket...) starts with
 * uw_begin; while it looks at its descriptors, their readiness checks note the objects
 * they read (uw_note: a pipe, a socket, an eventfd...).  If nothing is ready it sleeps
 * (uw_sleep) until one of those objects changes (uw_kick, from whoever changed it), a
 * time-based object is due, its own timeout, or a signal or stop wakes the thread.
 *
 * Before this every wait looked again every 1-5 ms: Ladybird's ~120 threads, nearly all
 * waiting, made ~11,000 needless switches a second on the phone's one core, and Linux
 * (Wi-Fi, the display) waited behind them (0.31.1).
 *
 * A kick between a waiter's look and its sleep is not lost: any kick in between (the
 * generation count moved) sends it round to look again.  On x86-64's several cores a
 * kick can still slip in between linking and sleeping, so a waiter there also looks
 * again after a short while; on ARM (one core, interrupts masked from link to sleep) the
 * backstop only covers changes nothing kicks for.
 */
#include "uwait.h"
#include "uarch.h"

const char uw_net_key[1];
static uwait_t *waiters;
static volatile u64 gen;
static volatile int lk;
#if defined(__x86_64__)
#define BACKSTOP_US 20000ull
static void lock(void) { while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause"); }
#else
#define BACKSTOP_US 250000ull
static void lock(void) { while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {} }
#endif
static void unlock(void) { __atomic_store_n(&lk, 0, __ATOMIC_RELEASE); }

void uw_begin(uwait_t *w) {
    thread_t *me = thread_current();
    w->t = me; w->n = 0; w->any = 0; w->deadline_us = ~0ull; w->next = NULL;
    w->gen = gen;
    me->uw = w;
    if (me->proc) uw_note(me->proc);                   /* signals to the process (signal.c kicks it) */
}

void uw_note(const void *key) {
    uwait_t *w = thread_current()->uw;
    if (!w || !key) return;
    for (int i = 0; i < w->n; i++) if (w->keys[i] == key) return;
    if (w->n < UW_KEYS) w->keys[w->n++] = key; else w->any = 1;
}

void uw_deadline(u64 us) {
    uwait_t *w = thread_current()->uw;
    if (w && us < w->deadline_us) w->deadline_us = us;
}

void uw_sleep(uwait_t *w, u64 end_us) {
    thread_t *me = thread_current();
    me->uw = NULL;
    u64 now = k_now_us();
    u64 until = MIN(MIN(end_us, w->deadline_us), now + BACKSTOP_US);
    if (until <= now) return;
    u64 fl = irq_save();
    lock();
    if (gen != w->gen) { unlock(); irq_restore(fl); return; }   /* something changed while it looked */
    w->next = waiters;
    waiters = w;
    unlock();
    u64 ms = (until - now + 999) / 1000;
    thread_sleep_ms(ms ? ms : 1);                      /* a kick wakes it early */
    lock();
    for (uwait_t **pp = &waiters; *pp; pp = &(*pp)->next) if (*pp == w) { *pp = w->next; break; }
    unlock();
    irq_restore(fl);
}

/* a thread stopped where it stands (exec, exit_group): its wait goes with it */
void uw_drop(thread_t *t) {
    u64 fl = irq_save();
    lock();
    for (uwait_t **pp = &waiters; *pp;) { if ((*pp)->t == t) *pp = (*pp)->next; else pp = &(*pp)->next; }
    unlock();
    irq_restore(fl);
}

void uw_end(void) { thread_current()->uw = NULL; }

void uw_kick(const void *key) {
    u64 fl = irq_save();
    lock();
    gen++;
    for (uwait_t *w = waiters; w; w = w->next) {
        int hit = w->any;
        for (int i = 0; i < w->n && !hit; i++) hit = w->keys[i] == key;
        if (hit) thread_wake(w->t);
    }
    unlock();
    irq_restore(fl);
}
