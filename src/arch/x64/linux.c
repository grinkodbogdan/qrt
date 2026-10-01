/*
 * linux.c - the Linux x86-64 system call ABI on top of the Tessera kernel.
 *
 * Enough of Linux for unmodified binaries, static or dynamically linked
 * (glibc's ld.so, musl, busybox, libstdc++): files and directories on the
 * VFS, memory (brk, anonymous and file-backed mmap with MAP_FIXED, munmap),
 * TLS (arch_prctl), threads (clone + futex), time, identity, and exit.  Unknown calls return -ENOSYS and
 * are logged once, so missing pieces are easy to find.  Like WSL1 or
 * FreeBSD's Linuxulator, this is the ABI, not the Linux kernel.
 */
#include "proc.h"
#include "lsock.h"
#include "../../net/crypto.h"
#include "mm.h"

enum { EPERM = 1, ENOENT = 2, EINTR = 4, EBADF = 9, ECHILD = 10, EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EEXIST = 17,
       ENOTDIR = 20, EISDIR = 21, EINVAL = 22, EMFILE = 24, ENOTTY = 25, ESPIPE = 29, ERANGE = 34,
       ENOSYS = 38, ENOTEMPTY = 39, ETIMEDOUT = 110 };

#define O_ACCMODE 3
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_DIRECTORY 0200000
#define AT_FDCWD  (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000

typedef struct {
    u64 st_dev, st_ino, st_nlink;
    u32 st_mode, st_uid, st_gid, pad0;
    u64 st_rdev;
    i64 st_size, st_blksize, st_blocks;
    u64 st_atime, st_atime_ns, st_mtime, st_mtime_ns, st_ctime, st_ctime_ns;
    i64 reserved[3];
} lstat_t;

/* ---- helpers --------------------------------------------------------------------- */
#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))

static int get_path(proc_t *p, u64 uptr, char *out, usize cap) {
    if (!UOK(uptr, 1)) return -EFAULT;
    const char *s = (const char *)(usize)uptr;
    usize i = 0;
    for (; i + 1 < cap; i++) {
        if (!UOK(uptr + i, 1)) return -EFAULT;
        out[i] = s[i];
        if (!s[i]) break;
    }
    out[cap - 1] = 0;
    return 0;
}

/* resolve relative to the cwd (or dirfd) into an absolute path */
static void abs_path(proc_t *p, int dirfd, const char *in, char *out, usize cap) {
    if (in[0] == '/') { strlcpy(out, in, cap); return; }
    char base[160] = "/";
    if (dirfd >= 0 && dirfd < MAX_FDS && p->fd[dirfd].type == F_DIR) vfs_path(p->fd[dirfd].vn, base, sizeof base);
    else strlcpy(base, p->cwd, sizeof base);
    fmt(out, cap, "%s/%s", base, in);
}

static u64 now_ns(void) { return k_now_us() * 1000; }
static u64 epoch_now(void) { return k.epoch_at_boot + k_now_ms() / 1000; }

static int alloc_fd(proc_t *p, int from) {
    for (int i = from; i < MAX_FDS; i++) if (p->fd[i].type == F_NONE) return i;
    return -EMFILE;
}

static void fill_stat(lstat_t *st, vnode_t *n, int tty) {
    memset(st, 0, sizeof *st);
    st->st_dev = 1;
    st->st_ino = (u64)(usize)n / 16;
    st->st_nlink = 1;
    if (tty) { st->st_mode = 0020620; st->st_rdev = 0x8800; }
    else if (!n) st->st_mode = 0020666;
    else st->st_mode = n->dir ? 0040755 : (0100644 | (n->size > 4 && !memcmp(n->data ? (char *)n->data : "", "\x7f" "ELF", 4) ? 0111 : 0));
    st->st_size = n && !n->dir ? (i64)vfs_size(n) : 0;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_atime = st->st_mtime = st->st_ctime = k.epoch_at_boot;
}

static i64 do_open(proc_t *p, int dirfd, u64 upath, int flags) {
    char path[160], full[256];
    int e = get_path(p, upath, path, sizeof path);
    if (e) return e;
    if (!strcmp(path, "/dev/null")) { int fd = alloc_fd(p, 0); if (fd >= 0) p->fd[fd] = (ufile_t){ F_NULL }; return fd; }
    if (!strcmp(path, "/dev/tty") || !strcmp(path, "/dev/console")) { int fd = alloc_fd(p, 0); if (fd >= 0) p->fd[fd] = (ufile_t){ F_TTY }; return fd; }
    abs_path(p, dirfd, path, full, sizeof full);
    vnode_t *n = vfs_lookup(full);
    if (!n) {
        if (!(flags & O_CREAT)) return -ENOENT;
        n = vfs_create(full, 0);
    } else if ((flags & O_TRUNC) && !n->dir) vfs_truncate(n);
    if ((flags & O_DIRECTORY) && !n->dir) return -ENOTDIR;
    int fd = alloc_fd(p, 0);
    if (fd < 0) return fd;
    p->fd[fd] = (ufile_t){ n->dir ? F_DIR : F_FILE, n, (flags & O_APPEND) ? vfs_size(n) : 0, flags, 0 };
    return fd;
}

static i64 do_write(proc_t *p, int fd, u64 buf, u64 len) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    ufile_t *f = &p->fd[fd];
    if (!UOK(buf, len)) return -EFAULT;
    switch (f->type) {
    case F_TTY: term_append(p->term, (const char *)(usize)buf, len); return (i64)len;
    case F_NULL: return (i64)len;
    case F_FILE: {
        i64 r = vfs_write(f->vn, f->off, (const void *)(usize)buf, len);
        if (r > 0) f->off += (u64)r;
        return r;
    }
    case F_DIR: return -EISDIR;
    case F_SOCK: return lsock_sendto(p, fd, buf, len, 0, 0, 0);
    default: return -EBADF;
    }
}

static i64 do_read(proc_t *p, int fd, u64 buf, u64 len) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    ufile_t *f = &p->fd[fd];
    if (!UOK(buf, len)) return -EFAULT;
    switch (f->type) {
    case F_TTY: case F_NULL: return 0;                  /* no keyboard for programs yet: EOF */
    case F_FILE: {
        i64 r = vfs_read(f->vn, f->off, (void *)(usize)buf, len);
        if (r > 0) f->off += (u64)r;
        return r;
    }
    case F_DIR: return -EISDIR;
    case F_SOCK: return lsock_recvfrom(p, fd, buf, len, 0, 0, 0);
    default: return -EBADF;
    }
}

/* ---- poll / select ---------------------------------------------------------------------- */
enum { POLLIN = 1, POLLPRI = 2, POLLOUT = 4, POLLERR = 8, POLLHUP = 0x10, POLLNVAL = 0x20 };

static int fd_ready(proc_t *p, int fd, int events) {
    if (fd < 0 || fd >= MAX_FDS || !p->fd[fd].type) return POLLNVAL;
    int rev = 0;
    switch (p->fd[fd].type) {
    case F_SOCK: {
        int hup = 0;
        if ((events & POLLIN) && lsock_readable(p, fd, &hup)) rev |= POLLIN;
        if (hup) rev |= POLLHUP;
        if ((events & POLLOUT) && lsock_writable(p, fd)) rev |= POLLOUT;
        break;
    }
    case F_TTY: rev = events & POLLOUT; break;            /* no keyboard input for programs */
    default: rev = events & (POLLIN | POLLOUT); break;
    }
    return rev;
}

static i64 do_poll(proc_t *p, u64 ufds, u64 n, i64 timeout_ms) {
    if (n > MAX_FDS * 4 || (n && !UOK(ufds, n * 8))) return -EINVAL;
    u64 end = timeout_ms < 0 ? ~0ull : k_now_ms() + (u64)timeout_ms;
    for (;;) {
        int count = 0;
        for (u64 i = 0; i < n; i++) {
            u8 *e = (u8 *)(usize)(ufds + i * 8);
            int fd = *(i32 *)e;
            i16 ev = *(i16 *)(e + 4);
            i16 rev = fd < 0 ? 0 : (i16)fd_ready(p, fd, ev);
            *(i16 *)(e + 6) = rev;
            if (rev) count++;
        }
        if (count || k_now_ms() >= end) return count;
        if (p->killed) return -EINTR;
        thread_sleep_ms(5);
    }
}

static i64 do_select(proc_t *p, int nfds, u64 rfds, u64 wfds, u64 efds, i64 timeout_ms) {
    if (nfds < 0 || nfds > MAX_FDS) nfds = MAX_FDS;
    u64 rin = 0, win = 0;
    if (rfds) { if (!UOK(rfds, 8)) return -EFAULT; rin = *(u64 *)(usize)rfds; }
    if (wfds) { if (!UOK(wfds, 8)) return -EFAULT; win = *(u64 *)(usize)wfds; }
    if (efds && UOK(efds, 8)) *(u64 *)(usize)efds = 0;
    u64 end = timeout_ms < 0 ? ~0ull : k_now_ms() + (u64)timeout_ms;
    for (;;) {
        u64 rout = 0, wout = 0;
        int count = 0;
        for (int fd = 0; fd < nfds; fd++) {
            int want = ((rin >> fd) & 1 ? POLLIN : 0) | ((win >> fd) & 1 ? POLLOUT : 0);
            if (!want) continue;
            int rev = fd_ready(p, fd, want);
            if (rev & POLLNVAL) return -EBADF;
            if (rev & (POLLIN | POLLHUP)) { rout |= 1ull << fd; count++; }
            if (rev & POLLOUT) { wout |= 1ull << fd; count++; }
        }
        if (count || k_now_ms() >= end) {
            if (rfds) *(u64 *)(usize)rfds = rout;
            if (wfds) *(u64 *)(usize)wfds = wout;
            return count;
        }
        if (p->killed) return -EINTR;
        thread_sleep_ms(5);
    }
}

static i64 do_getdents(proc_t *p, int fd, u64 buf, u64 len) {
    if (fd < 0 || fd >= MAX_FDS || p->fd[fd].type != F_DIR) return -ENOTDIR;
    if (!UOK(buf, len)) return -EFAULT;
    ufile_t *f = &p->fd[fd];
    u8 *out = (u8 *)(usize)buf;
    u64 pos = 0;
    for (;;) {
        const char *name;
        u8 type;
        vnode_t *c = NULL;
        if (f->dir_index == 0) { name = "."; type = 4; }
        else if (f->dir_index == 1) { name = ".."; type = 4; }
        else {
            c = vfs_child_at(f->vn, f->dir_index - 2);
            if (!c) break;
            name = c->name;
            type = c->dir ? 4 : 8;
        }
        u64 nl = strlen(name), rec = (19 + nl + 1 + 7) & ~7ull;
        if (pos + rec > len) { if (!pos) return -EINVAL; break; }
        u8 *d = out + pos;
        *(u64 *)d = c ? (u64)(usize)c / 16 : (u64)f->dir_index + 1;
        *(i64 *)(d + 8) = f->dir_index + 1;
        *(u16 *)(d + 16) = (u16)rec;
        d[18] = type;
        memcpy(d + 19, name, nl + 1);
        pos += rec;
        f->dir_index++;
    }
    return (i64)pos;
}

static i64 do_stat_path(proc_t *p, int dirfd, u64 upath, u64 ust, int flags) {
    char path[160], full[256];
    if (!UOK(ust, sizeof(lstat_t))) return -EFAULT;
    int e = get_path(p, upath, path, sizeof path);
    if (e) return e;
    lstat_t *st = (lstat_t *)(usize)ust;
    if (!path[0] && (flags & AT_EMPTY_PATH)) {
        if (dirfd < 0 || dirfd >= MAX_FDS || p->fd[dirfd].type == F_NONE) return -EBADF;
        fill_stat(st, p->fd[dirfd].vn, p->fd[dirfd].type == F_TTY);
        return 0;
    }
    if (!strcmp(path, "/dev/null")) { fill_stat(st, NULL, 0); return 0; }
    abs_path(p, dirfd, path, full, sizeof full);
    vnode_t *n = vfs_lookup(full);
    if (!n) return -ENOENT;
    fill_stat(st, n, 0);
    return 0;
}

#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_FIXED_NOREPLACE 0x100000

static i64 do_mmap(proc_t *p, u64 addr, u64 len, u64 prot, u64 flags, i64 fd, u64 off) {
    (void)prot;                                            /* no page protections yet: all user pages are RW */
    if (!len || (off & (PAGE - 1))) return -EINVAL;
    len = (len + PAGE - 1) & ~(PAGE - 1);
    int anon = (flags & MAP_ANONYMOUS) || fd < 0;
    if (!anon && (fd >= MAX_FDS || p->fd[fd].type != F_FILE)) return -EBADF;
    u64 va;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (addr & (PAGE - 1)) return -EINVAL;
        if (addr < PAGE || addr + len > USER_STACK_TOP - USER_STACK_SIZE) return -ENOMEM;
        va = addr;
        if ((flags & MAP_FIXED_NOREPLACE) && !(flags & MAP_FIXED)) {
            for (int i = 0; i < p->nvma; i++) if (va < p->vma[i].end && p->vma[i].start < va + len) return -EEXIST;
        }
        proc_unmap(p, va, va + len);                       /* MAP_FIXED replaces what was there */
    } else {
        u64 hint = addr & ~(PAGE - 1);
        va = hint && proc_range_free(p, hint, hint + len) ? hint : proc_find_free(p, len);
        if (!va) return -ENOMEM;
    }
    if (proc_add_vma(p, va, va + len)) return -ENOMEM;
    if (!anon) {                                           /* file mapping: a private copy */
        vnode_t *vn = p->fd[fd].vn;
        u64 fsz = vfs_size(vn);
        if (off < fsz) {
            u64 n = MIN(len, fsz - off);
            if (!UOK(va, n)) return -ENOMEM;
            vfs_read(vn, off, (void *)(usize)va, n);
        }
    }
    return (i64)va;
}

/* ---- futex: the wait queue threads sleep on (pthread mutexes, condvars, join) ---------- */
typedef struct waiter { proc_t *p; u64 addr; thread_t *t; volatile int woken; struct waiter *next; } waiter_t;
static waiter_t *waiters;                                  /* all user threads run on the boot core */

i64 futex_wake(proc_t *p, u64 uaddr, int n) {
    int woke = 0;
    u64 fl = irq_save();
    for (waiter_t *w = waiters; w && woke < n; w = w->next)
        if (w->p == p && w->addr == uaddr && !w->woken) { w->woken = 1; thread_wake(w->t); woke++; }
    irq_restore(fl);
    return woke;
}

static int futex_requeue(proc_t *p, u64 from, u64 to, int wake, int move) {
    int woke = 0, moved = 0;
    u64 fl = irq_save();
    for (waiter_t *w = waiters; w; w = w->next) {
        if (w->p != p || w->addr != from || w->woken) continue;
        if (woke < wake) { w->woken = 1; thread_wake(w->t); woke++; }
        else if (moved < move) { w->addr = to; moved++; }
    }
    irq_restore(fl);
    return woke + moved;
}

/* deadline_us: 0 = forever, else k_now_us() time */
static i64 futex_wait(proc_t *p, u64 uaddr, u32 val, u64 deadline_us) {
    if (!UOK(uaddr, 4)) return -EFAULT;
    waiter_t w = { p, uaddr, thread_current(), 0, NULL };
    u64 fl = irq_save();
    if (*(volatile u32 *)(usize)uaddr != val) { irq_restore(fl); return -EAGAIN; }
    w.next = waiters;
    waiters = &w;
    i64 r = 0;
    for (;;) {
        if (w.woken) break;
        if (p->killed) { r = -EINTR; break; }
        u64 now = k_now_us(), ms = 50;
        if (deadline_us) {
            if (now >= deadline_us) { r = -ETIMEDOUT; break; }
            ms = MIN(ms, (deadline_us - now + 999) / 1000);
        }
        thread_sleep_ms(ms);                               /* futex_wake ends the sleep early */
    }
    for (waiter_t **pp = &waiters; *pp; pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; break; }
    irq_restore(fl);
    return r;
}

static i64 do_futex(proc_t *p, u64 uaddr, int op, u32 val, u64 utime, u64 uaddr2, u32 val3) {
    int cmd = op & 0x7f, realtime = op & 256;
    switch (cmd) {
    case 0: case 9: {                                      /* WAIT (relative), WAIT_BITSET (absolute) */
        u64 deadline = 0;
        if (utime) {
            if (!UOK(utime, 16)) return -EFAULT;
            const i64 *ts = (const i64 *)(usize)utime;
            u64 ns = (u64)ts[0] * 1000000000ull + (u64)ts[1];
            if (cmd == 0) deadline = k_now_us() + ns / 1000 + 1;
            else if (realtime) {
                u64 rt_now = epoch_now() * 1000000000ull + (k_now_us() % 1000000) * 1000;
                deadline = ns > rt_now ? k_now_us() + (ns - rt_now) / 1000 + 1 : k_now_us();
            } else deadline = ns / 1000 + 1;                /* CLOCK_MONOTONIC is k_now_us based */
            if (!deadline) deadline = 1;
        }
        return futex_wait(p, uaddr, val, deadline);
    }
    case 1: case 10: return futex_wake(p, uaddr, (int)val);           /* WAKE, WAKE_BITSET */
    case 3: return futex_requeue(p, uaddr, uaddr2, (int)val, (int)utime);  /* REQUEUE */
    case 4:                                                 /* CMP_REQUEUE */
        if (!UOK(uaddr, 4)) return -EFAULT;
        if (*(volatile u32 *)(usize)uaddr != val3) return -EAGAIN;
        return futex_requeue(p, uaddr, uaddr2, (int)val, (int)utime);
    case 5: {                                               /* WAKE_OP: the op on uaddr2, then wake both */
        if (!UOK(uaddr2, 4)) return -EFAULT;
        u32 *u2 = (u32 *)(usize)uaddr2, old = *u2;
        u32 opk = (val3 >> 28) & 7, cmp = (val3 >> 24) & 15, oparg = (val3 >> 12) & 0xfff, cmparg = val3 & 0xfff;
        if (val3 & (8u << 28)) oparg = 1u << oparg;
        switch (opk) { case 0: *u2 = oparg; break; case 1: *u2 += oparg; break; case 2: *u2 |= oparg; break;
                       case 3: *u2 &= ~oparg; break; case 4: *u2 ^= oparg; break; }
        int c = 0;
        switch (cmp) { case 0: c = old == cmparg; break; case 1: c = old != cmparg; break; case 2: c = (i32)old < (i32)cmparg; break;
                       case 3: c = (i32)old <= (i32)cmparg; break; case 4: c = (i32)old > (i32)cmparg; break; case 5: c = (i32)old >= (i32)cmparg; break; }
        i64 n = futex_wake(p, uaddr, (int)val);
        if (c) n += futex_wake(p, uaddr2, (int)utime);
        return n;
    }
    default: return -ENOSYS;                                /* PI futexes */
    }
}

static i64 do_brk(proc_t *p, u64 want) {
    if (want < p->brk_start || want >= USER_MMAP_BASE) return (i64)p->brk;
    want = (want + PAGE - 1) & ~(PAGE - 1);
    if (want > p->brk) proc_add_vma(p, p->brk, want);
    p->brk = want;
    return (i64)p->brk;
}

static i64 do_uname(proc_t *p, u64 buf) {
    if (!UOK(buf, 65 * 6)) return -EFAULT;
    char *u = (char *)(usize)buf;
    memset(u, 0, 65 * 6);
    strlcpy(u + 0, "Linux", 65);                        /* the ABI we speak */
    strlcpy(u + 65, k.is_venue ? "venue" : "qrt", 65);
    strlcpy(u + 130, "6.1.0-qrt", 65);                  /* static glibc refuses kernels < 3.2 */
    strlcpy(u + 195, "#1 QRT " QRT_VERSION " (Tessera)", 65);
    strlcpy(u + 260, "x86_64", 65);
    strlcpy(u + 325, "(none)", 65);
    return 0;
}

static void log_unknown(u64 nr) {
    static u64 seen[8];
    for (int i = 0; i < 8; i++) { if (seen[i] == nr + 1) return; if (!seen[i]) { seen[i] = nr + 1; break; } }
    klog("linux: unimplemented system call %llu", nr);
}

/* ---- dispatch ----------------------------------------------------------------------- */
void syscall_dispatch(frame_t *f) {
    proc_t *p = proc_current();
    u64 nr = f->rax, a0 = f->rdi, a1 = f->rsi, a2 = f->rdx, a3 = f->r10, a4 = f->r8, a5 = f->r9;
    i64 r = -ENOSYS;
    thread_t *me = thread_current();
    p->syscalls++;
    me->in_sys = 1;
    switch (nr) {
    case 0:  r = do_read(p, (int)a0, a1, a2); break;
    case 1:  r = do_write(p, (int)a0, a1, a2); break;
    case 2:  r = do_open(p, AT_FDCWD, a0, (int)a1); break;
    case 257: r = do_open(p, (int)a0, a1, (int)a2); break;
    case 3:
        if ((int)a0 >= 0 && (int)a0 < MAX_FDS && p->fd[a0].type) {
            if (p->fd[a0].type == F_SOCK) lsock_close(p, (int)a0); else p->fd[a0].type = F_NONE;
            r = 0;
        } else r = -EBADF;
        break;
    /* sockets (lsock.c) */
    case 41: r = lsock_socket(p, (int)a0, (int)a1, (int)a2); break;
    case 42: r = lsock_connect(p, (int)a0, a1, a2); break;
    case 43: case 288: case 50: case 53: r = -95; break;          /* accept, accept4, listen, socketpair: EOPNOTSUPP */
    case 44: r = lsock_sendto(p, (int)a0, a1, a2, (int)a3, a4, a5); break;
    case 45: r = lsock_recvfrom(p, (int)a0, a1, a2, (int)a3, a4, a5); break;
    case 46: r = lsock_sendmsg(p, (int)a0, a1, (int)a2); break;
    case 47: r = lsock_recvmsg(p, (int)a0, a1, (int)a2); break;
    case 48: r = lsock_shutdown(p, (int)a0, (int)a1); break;
    case 49: r = lsock_bind(p, (int)a0, a1, a2); break;
    case 51: r = lsock_getname(p, (int)a0, a1, a2, 0); break;
    case 52: r = lsock_getname(p, (int)a0, a1, a2, 1); break;
    case 54: r = 0; break;                                        /* setsockopt: accepted */
    case 55: r = lsock_getsockopt(p, (int)a0, (int)a1, (int)a2, a3, a4); break;
    case 307: r = lsock_sendmmsg(p, (int)a0, a1, (u32)a2, (int)a3); break;
    case 7: r = do_poll(p, a0, a1, (i64)(i32)a2); break;
    case 37: r = 0; break;                                        /* alarm: no signals yet, so no timer */
    case 77:                                                      /* ftruncate */
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || p->fd[a0].type != F_FILE) { r = -EBADF; break; }
        if (a1 <= p->fd[a0].vn->size) p->fd[a0].vn->size = a1;
        else { static const u8 z[512]; u64 at = p->fd[a0].vn->size; while (at < a1) { u64 n = MIN(a1 - at, (u64)sizeof z); vfs_write(p->fd[a0].vn, at, z, n); at += n; } }
        r = 0;
        break;
    case 271: {                                                   /* ppoll: timespec */
        i64 ms = -1;
        if (a2) { if (!UOK(a2, 16)) { r = -EFAULT; break; } u64 *ts = (u64 *)(usize)a2; ms = (i64)(ts[0] * 1000 + ts[1] / 1000000); }
        r = do_poll(p, a0, a1, ms);
        break;
    }
    case 23: case 270: {                                          /* select (timeval), pselect6 (timespec) */
        i64 ms = -1;
        if (a4) {
            if (!UOK(a4, 16)) { r = -EFAULT; break; }
            u64 *t = (u64 *)(usize)a4;
            ms = nr == 23 ? (i64)(t[0] * 1000 + t[1] / 1000) : (i64)(t[0] * 1000 + t[1] / 1000000);
        }
        r = do_select(p, (int)a0, a1, a2, a3, ms);
        break;
    }
    case 4:  r = do_stat_path(p, AT_FDCWD, a0, a1, 0); break;
    case 6:  r = do_stat_path(p, AT_FDCWD, a0, a1, AT_SYMLINK_NOFOLLOW); break;
    case 262: r = do_stat_path(p, (int)a0, a1, a2, (int)a3); break;
    case 5:
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || !p->fd[a0].type) { r = -EBADF; break; }
        if (!UOK(a1, sizeof(lstat_t))) { r = -EFAULT; break; }
        fill_stat((lstat_t *)(usize)a1, p->fd[a0].vn, p->fd[a0].type == F_TTY);
        r = 0; break;
    case 8: {                                           /* lseek */
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || p->fd[a0].type != F_FILE) { r = p->fd[a0 & 31].type == F_TTY ? -ESPIPE : -EBADF; break; }
        ufile_t *uf = &p->fd[a0];
        i64 base = a2 == 0 ? 0 : a2 == 1 ? (i64)uf->off : (i64)vfs_size(uf->vn);
        i64 no = base + (i64)a1;
        if (no < 0) { r = -EINVAL; break; }
        uf->off = (u64)no; r = no; break;
    }
    case 17:                                            /* pread64 */
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || p->fd[a0].type != F_FILE) { r = -EBADF; break; }
        if (!UOK(a1, a2)) { r = -EFAULT; break; }
        r = vfs_read(p->fd[a0].vn, a3, (void *)(usize)a1, a2); break;
    case 19: case 20: {                                 /* readv / writev */
        if (!UOK(a1, a2 * 16)) { r = -EFAULT; break; }
        const u64 *iov = (const u64 *)(usize)a1;
        r = 0;
        for (u64 i = 0; i < a2; i++) {
            i64 n = nr == 20 ? do_write(p, (int)a0, iov[i * 2], iov[i * 2 + 1]) : do_read(p, (int)a0, iov[i * 2], iov[i * 2 + 1]);
            if (n < 0) { if (!r) r = n; break; }
            r += n;
            if ((u64)n < iov[i * 2 + 1]) break;
        }
        break;
    }
    case 217: r = do_getdents(p, (int)a0, a1, a2); break;
    case 9:  r = do_mmap(p, a0, a1, a2, a3, (i64)a4, a5); break;
    case 11:                                            /* munmap */
        if (a0 & (PAGE - 1)) { r = -EINVAL; break; }
        proc_unmap(p, a0, (a0 + a1 + PAGE - 1) & ~(PAGE - 1));
        r = 0; break;
    case 25: r = -ENOMEM; break;                        /* mremap: glibc falls back to malloc + copy */
    case 10: case 28: r = 0; break;                     /* mprotect, madvise: accepted */
    case 12: r = do_brk(p, a0); break;
    case 158:                                           /* arch_prctl */
        if (a0 == 0x1002) { thread_current()->fs_base = a1; wrmsr(MSR_FS_BASE, a1); r = 0; }
        else if (a0 == 0x1003) { if (UOK(a1, 8)) { *(u64 *)(usize)a1 = thread_current()->fs_base; r = 0; } else r = -EFAULT; }
        else r = -EINVAL;
        break;
    case 218: me->clear_tid = a0; r = me->tid; break;   /* set_tid_address */
    case 273: case 13: case 14: case 131: r = 0; break; /* robust list, signals, sigaltstack: accepted */
    case 334: r = -ENOSYS; break;                       /* rseq: glibc copes */
    case 16:                                            /* ioctl */
        if ((int)a0 >= 0 && (int)a0 < MAX_FDS && p->fd[a0].type == F_SOCK) {
            if (a1 == 0x541b && UOK(a2, 4)) { *(i32 *)(usize)a2 = (i32)lsock_available(p, (int)a0); r = 0; }      /* FIONREAD */
            else if (a1 == 0x5421 && UOK(a2, 4)) { lsock_set_nonblock(p, (int)a0, *(i32 *)(usize)a2 != 0); r = 0; } /* FIONBIO */
            else r = -ENOTTY;
        } else r = -ENOTTY;                             /* no terminal control */
        break;
    case 72:                                            /* fcntl */
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || !p->fd[a0].type) { r = -EBADF; break; }
        if (a1 == 0 || a1 == 1030) { int fd = alloc_fd(p, (int)a2); if (fd >= 0) { p->fd[fd] = p->fd[a0]; if (p->fd[fd].type == F_SOCK) lsock_dup(p, fd); } r = fd; }
        else if (a1 == 3) r = p->fd[a0].type == F_SOCK ? 2 | (p->fd[a0].flags & 04000) : (p->fd[a0].flags & O_ACCMODE ? p->fd[a0].flags : 2);
        else if (a1 == 4) {                             /* F_SETFL: O_NONBLOCK on sockets */
            p->fd[a0].flags = (p->fd[a0].flags & ~04000) | ((int)a2 & 04000);
            if (p->fd[a0].type == F_SOCK) lsock_set_nonblock(p, (int)a0, (a2 & 04000) != 0);
            r = 0;
        }
        else r = 0;
        break;
    case 32: {
        int fd = alloc_fd(p, 0);
        if (fd >= 0 && (int)a0 >= 0 && (int)a0 < MAX_FDS) { p->fd[fd] = p->fd[a0]; if (p->fd[fd].type == F_SOCK) lsock_dup(p, fd); }
        r = fd;
        break;
    }
    case 33: case 292:
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || (int)a1 < 0 || (int)a1 >= MAX_FDS) { r = -EBADF; break; }
        if (a0 != a1) {
            if (p->fd[a1].type == F_SOCK) lsock_close(p, (int)a1);
            p->fd[a1] = p->fd[a0];
            if (p->fd[a1].type == F_SOCK) lsock_dup(p, (int)a1);
        }
        r = (i64)a1; break;
    case 21: case 269: case 439: {                      /* access, faccessat, faccessat2 */
        char path[160], full[256];
        int dirfd = nr == 21 ? AT_FDCWD : (int)a0;
        r = get_path(p, nr == 21 ? a0 : a1, path, sizeof path);
        if (r) break;
        abs_path(p, dirfd, path, full, sizeof full);
        r = vfs_lookup(full) ? 0 : -ENOENT;
        break;
    }
    case 79: {                                          /* getcwd */
        usize n = strlen(p->cwd) + 1;
        if (a1 < n) { r = -ERANGE; break; }
        if (!UOK(a0, n)) { r = -EFAULT; break; }
        memcpy((void *)(usize)a0, p->cwd, n); r = (i64)n; break;
    }
    case 80: {                                          /* chdir */
        char path[160], full[256];
        r = get_path(p, a0, path, sizeof path);
        if (r) break;
        abs_path(p, AT_FDCWD, path, full, sizeof full);
        vnode_t *n = vfs_lookup(full);
        if (!n) r = -ENOENT;
        else if (!n->dir) r = -ENOTDIR;
        else { vfs_path(n, p->cwd, sizeof p->cwd); r = 0; }
        break;
    }
    case 89: case 267: {                                /* readlink(at): only /proc/self/exe */
        char path[160];
        r = get_path(p, nr == 89 ? a0 : a1, path, sizeof path);
        if (r) break;
        u64 buf = nr == 89 ? a1 : a2, cap = nr == 89 ? a2 : a3;
        if (strcmp(path, "/proc/self/exe")) { r = -EINVAL; break; }
        usize n = MIN(strlen(p->exe), cap);
        if (!UOK(buf, n)) { r = -EFAULT; break; }
        memcpy((void *)(usize)buf, p->exe, n); r = (i64)n; break;
    }
    case 83: {                                          /* mkdir */
        char path[160], full[256];
        r = get_path(p, a0, path, sizeof path);
        if (r) break;
        abs_path(p, AT_FDCWD, path, full, sizeof full);
        r = vfs_lookup(full) ? -EEXIST : (vfs_create(full, 1) ? 0 : -ENOENT);
        break;
    }
    case 63: r = do_uname(p, a0); break;
    case 39: r = p->pid; break;
    case 186: r = me->tid; break;                       /* gettid */
    case 200: case 234: r = 0; break;                   /* tkill, tgkill: no signals yet */
    case 110: r = 1; break;
    case 102: case 104: case 107: case 108: r = 0; break;       /* root */
    case 95: r = 022; break;                                    /* umask */
    case 97: case 302: {                                        /* getrlimit / prlimit64 */
        u64 out = nr == 97 ? a1 : a3, res = nr == 97 ? a0 : a1;
        if (!out) { r = 0; break; }
        if (!UOK(out, 16)) { r = -EFAULT; break; }
        u64 *rl = (u64 *)(usize)out;
        rl[0] = rl[1] = res == 3 ? USER_STACK_SIZE : ~0ull;     /* RLIMIT_STACK */
        r = 0; break;
    }
    case 204:                                                   /* sched_getaffinity */
        if (!UOK(a2, 8)) { r = -EFAULT; break; }
        *(u64 *)(usize)a2 = 1; r = 8; break;
    case 24: thread_yield(); r = 0; break;
    case 99: {                                                  /* sysinfo */
        struct { i64 uptime; u64 loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
                 u16 procs, pad; u32 pad2; u64 totalhigh, freehigh; u32 mem_unit; char f[4]; } si = { 0 };
        si.uptime = (i64)(k_now_ms() / 1000);
        si.totalram = pmm_total_bytes();
        si.freeram = pmm_free_bytes();
        si.procs = 1;
        si.mem_unit = 1;
        if (!proc_user_ok(p, a0, sizeof si)) { r = -EFAULT; break; }
        memcpy((void *)(usize)a0, &si, sizeof si);
        r = 0;
        break;
    }
    case 157: r = 0; break;                                     /* prctl: accepted (names, dumpable...) */
    case 40: r = -EINVAL; break;                                /* sendfile: callers fall back to read/write */
    case 35: case 230: {                                        /* nanosleep, clock_nanosleep */
        u64 ts = nr == 35 ? a0 : a2;
        if (!UOK(ts, 16)) { r = -EFAULT; break; }
        u64 *t = (u64 *)(usize)ts;
        u64 end = k_now_ms() + t[0] * 1000 + t[1] / 1000000;
        r = 0;
        while (k_now_ms() < end) {                              /* in steps, so Stop works */
            if (p->killed) { r = -EINTR; break; }
            thread_sleep_ms(MIN(end - k_now_ms(), (u64)20));
        }
        break;
    }
    case 228: case 229: {                                       /* clock_gettime, clock_getres */
        if (!a1) { r = 0; break; }
        if (!UOK(a1, 16)) { r = -EFAULT; break; }
        u64 *t = (u64 *)(usize)a1;
        if (nr == 229) { t[0] = 0; t[1] = 1000; }
        else if (a0 == 0) { t[0] = epoch_now(); t[1] = (k_now_us() % 1000000) * 1000; }
        else { u64 ns = now_ns(); t[0] = ns / 1000000000; t[1] = ns % 1000000000; }
        r = 0; break;
    }
    case 96:                                                    /* gettimeofday */
        if (a0 && UOK(a0, 16)) { u64 *t = (u64 *)(usize)a0; t[0] = epoch_now(); t[1] = k_now_us() % 1000000; }
        r = 0; break;
    case 201:                                                   /* time */
        if (a0 && UOK(a0, 8)) *(u64 *)(usize)a0 = epoch_now();
        r = (i64)epoch_now(); break;
    case 318:                                                   /* getrandom */
        if (!UOK(a0, a1)) { r = -EFAULT; break; }
        random_bytes((u8 *)(usize)a0, (usize)a1);
        r = (i64)a1; break;
    case 202: r = do_futex(p, a0, (int)a1, (u32)a2, a3, a4, (u32)a5); break;
    case 56: r = proc_clone(p, f, a0, a1, a2, a3, a4); break;   /* clone: threads */
    case 435: r = -ENOSYS; break;                               /* clone3: glibc falls back to clone */
    case 57: case 58: case 59: r = -ENOSYS; break;              /* fork/vfork/execve: not yet */
    case 61: r = -ECHILD; break;                                /* wait4 */
    case 60: me->in_sys = 0; proc_thread_exit((int)(a0 & 0xff)); break;   /* exit: this thread */
    case 231: me->in_sys = 0; proc_exit((int)(a0 & 0xff)); break;         /* exit_group */
    default: log_unknown(nr); break;
    }
    f->rax = (u64)r;
    me->in_sys = 0;
    if (p->killed) proc_thread_exit(137);                       /* the process is going (Stop, exit_group) */
}
