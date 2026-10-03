/* netstack.c - one call per frame from the shell drives every network layer. */
#include "netstack.h"
#include "net.h"
#include "wlan.h"
#include "../drivers/e1000.h"
#if defined(__x86_64__)
#include "../arch/x64/sched.h"
#endif

/* The stack is single-threaded; the shell thread polls it and Linux
 * programs' socket calls enter it, so both take this lock. */
static volatile int lock;
static void *owner;                               /* the thread holding it (native) */

void net_lock(void) {
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        if (k.native) thread_yield(); else
#endif
        __asm__ volatile("pause");
    }
#if defined(__x86_64__)
    if (k.native) owner = thread_current();
#endif
}
void net_unlock(void) { owner = NULL; __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

/* a thread stopped in the middle of a system call (a kernel fault, proc.c) gives the lock back */
void net_lock_forfeit(void *thread) { if (thread && owner == thread) net_unlock(); }

void netstack_poll(void) {
    net_lock();
    e1000_poll();
    wlan_poll();
    net_poll();
    net_unlock();
    net_time_http_poll();
}

/* the home screen's network line */
const char *shell_net_status(void) { return net_status(); }

int netstack_kind(void) {
    netif_t *n = net_primary();
    if (n && n->ip) return strcmp(n->name, "Wi-Fi") ? 1 : 2;
    return wlan_state() != WL_OFF ? 3 : 0;
}
