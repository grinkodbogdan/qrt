/* netstack.h - glue: poll every network layer from the shell loop; the lock
 * that makes the stack safe to call from Linux programs' system calls. */
#pragma once
void netstack_poll(void);
void net_lock(void);
void net_unlock(void);
/* for the top bar: 0 = offline, 1 = wired, 2 = Wi-Fi connected, 3 = Wi-Fi on but not connected */
int netstack_kind(void);
void net_lock_forfeit(void *thread);   /* release the network lock if this thread holds it */
