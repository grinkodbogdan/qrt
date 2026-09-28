/* lsock.h - Linux AF_INET sockets over QRT's network stack (see lsock.c). */
#pragma once
#include "proc.h"

i64  lsock_socket(proc_t *p, int domain, int type, int proto);
i64  lsock_bind(proc_t *p, int fd, u64 addr, u64 len);
i64  lsock_connect(proc_t *p, int fd, u64 addr, u64 len);
i64  lsock_sendto(proc_t *p, int fd, u64 buf, u64 len, int flags, u64 addr, u64 alen);
i64  lsock_recvfrom(proc_t *p, int fd, u64 buf, u64 len, int flags, u64 addr, u64 alenp);
i64  lsock_sendmsg(proc_t *p, int fd, u64 msg, int flags);
i64  lsock_sendmmsg(proc_t *p, int fd, u64 vec, u32 n, int flags);
i64  lsock_recvmsg(proc_t *p, int fd, u64 msg, int flags);
i64  lsock_shutdown(proc_t *p, int fd, int how);
i64  lsock_getname(proc_t *p, int fd, u64 addr, u64 lenp, int peer);
i64  lsock_getsockopt(proc_t *p, int fd, int level, int opt, u64 val, u64 lenp);
int  lsock_readable(proc_t *p, int fd, int *hup);
int  lsock_writable(proc_t *p, int fd);
i64  lsock_available(proc_t *p, int fd);
void lsock_set_nonblock(proc_t *p, int fd, int on);
void lsock_close(proc_t *p, int fd);
void lsock_dup(proc_t *p, int fd);
void lsock_exit(proc_t *p);
