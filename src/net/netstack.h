/* netstack.h - glue: poll every network layer from the shell loop; the lock
 * that makes the stack safe to call from Linux programs' system calls. */
#pragma once
void netstack_poll(void);
void net_lock(void);
void net_unlock(void);
