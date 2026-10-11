/* uwait.h - waiting for something to happen (uwait.c): poll, select, epoll, pipes,
 * sockets, eventfd... sleep until whatever they wait on changes, instead of looking again
 * every few milliseconds. */
#pragma once
#include "proc.h"

#define UW_KEYS 24
typedef struct uwait {
    thread_t *t;
    const void *keys[UW_KEYS];       /* the objects whose change ends the wait */
    int n, any;                      /* any: more objects than keys - any change ends it */
    u64 deadline_us;                 /* the earliest time-based readiness (a timerfd) */
    u64 gen;
    struct uwait *next;
} uwait_t;

extern const char uw_net_key[];      /* the IP stack's sockets: one key (lsock.c, netstack.c) */
void uw_begin(uwait_t *w);           /* before looking: readiness checks note their objects */
void uw_note(const void *key);       /* this object's change would end the wait (no-op outside one) */
void uw_deadline(u64 us);            /* ... and so would this time (k_now_us()) */
void uw_sleep(uwait_t *w, u64 end_us);   /* until a change, the deadline, end_us (~0: none) or a wakeup */
void uw_drop(thread_t *t);           /* the thread is being stopped */
void uw_end(void);                   /* done looking without sleeping */
void uw_kick(const void *key);       /* the object changed: wake whoever waits on it */
