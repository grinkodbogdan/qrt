/* lfile.h - descriptor-backed kernel objects of the Linux layer (lfile.c, unix.c). */
#pragma once
#include "proc.h"

/* linux.c helpers the objects use */
int  fd_alloc(proc_t *p, int from);                       /* lowest free descriptor >= from, or -EMFILE */
int  fd_poll(proc_t *p, int fd, int events);              /* poll() readiness of one descriptor */
void ufile_ref(ufile_t *f);                               /* one more holder of this open file (dup, fork, in flight) */
void ufile_unref(ufile_t *f);
void path_abs(proc_t *p, int dirfd, const char *in, char *out, usize cap);
i64  fd_install_obj(proc_t *p, kobj_t *o, int flags);     /* a new descriptor for o (O_NONBLOCK, O_CLOEXEC honoured); takes the reference */

/* shared memory: memfd_create, /dev/shm (shm_open), MAP_SHARED */
kobj_t *shm_new(const char *name);
kobj_t *shm_wrap(u64 bytes, void **mem);                  /* contiguous kernel memory as a shared object */
i64  map_shared(proc_t *p, kobj_t *o, u64 len);          /* map o read-write somewhere free (linux.c) */
u64  shm_frame(kobj_t *o, u64 page);                      /* the frame of a page, allocated on first use (0: beyond the size) */
u64  shm_size(kobj_t *o);
i64  shm_truncate(kobj_t *o, u64 size);
i64  shm_rw(kobj_t *o, u64 off, void *buf, u64 len, int write);
kobj_t *shm_named(const char *name, int create, int excl, i64 *err);   /* /dev/shm/<name> */
i64  shm_unlink(const char *name);
int  shm_named_exists(const char *name);
int  shm_list(int i, char *name, usize cap, u64 *size); /* for getdents on /dev/shm */

/* eventfd, timerfd, signalfd */
kobj_t *eventfd_new(u64 init, int semaphore);
kobj_t *timerfd_new(int clock);
i64  timerfd_settime(proc_t *p, kobj_t *o, int flags, u64 nv, u64 ov);
i64  timerfd_gettime(proc_t *p, kobj_t *o, u64 cur);
kobj_t *signalfd_new(u64 mask);
void signalfd_set(kobj_t *o, u64 mask);

/* epoll */
kobj_t *epoll_new(void);
i64  epoll_ctl(proc_t *p, kobj_t *ep, int op, int fd, u64 ev);
i64  epoll_wait(proc_t *p, kobj_t *ep, u64 events, int max, i64 timeout_ms);

/* any object: read/write/poll as a descriptor */
i64  kobj_read(proc_t *p, ufile_t *f, u64 buf, u64 len);
i64  kobj_write(proc_t *p, ufile_t *f, u64 buf, u64 len);
int  kobj_poll(proc_t *p, ufile_t *f, int events);

/* Unix domain sockets (unix.c) */
i64  unix_socket(proc_t *p, int type, int flags);
i64  unix_socketpair(proc_t *p, int type, int flags, u64 sv);
i64  unix_bind(proc_t *p, ufile_t *f, u64 addr, u64 len);
i64  unix_listen(proc_t *p, ufile_t *f, int backlog);
i64  unix_accept(proc_t *p, ufile_t *f, u64 addr, u64 lenp, int flags);
i64  unix_connect(proc_t *p, ufile_t *f, u64 addr, u64 len);
i64  unix_sendto(proc_t *p, ufile_t *f, u64 buf, u64 len, int flags, u64 addr, u64 alen);
i64  unix_recvfrom(proc_t *p, ufile_t *f, u64 buf, u64 len, int flags, u64 addr, u64 alenp);
i64  unix_sendmsg(proc_t *p, ufile_t *f, u64 msg, int flags);
i64  unix_recvmsg(proc_t *p, ufile_t *f, u64 msg, int flags);
i64  unix_shutdown(proc_t *p, ufile_t *f, int how);
i64  unix_getname(proc_t *p, ufile_t *f, u64 addr, u64 lenp, int peer);
i64  unix_getsockopt(proc_t *p, ufile_t *f, int level, int opt, u64 val, u64 lenp);
int  unix_poll(proc_t *p, ufile_t *f, int events);
i64  unix_available(ufile_t *f);
void unix_release(kobj_t *o);
void unix_unlink_path(const char *path);                  /* unlink() of a socket's file */
