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

void net_lock(void) {
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        if (k.native) thread_yield(); else
#endif
        __asm__ volatile("pause");
    }
}
void net_unlock(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

void netstack_poll(void) {
    net_lock();
    e1000_poll();
    wlan_poll();
    net_poll();
    net_unlock();
}

/* the home screen's network line */
const char *shell_net_status(void) { return net_status(); }
