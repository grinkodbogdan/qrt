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
#include "lfile.h"
#include "native_sys.h"
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
#define KMSG      (1 << 30)
#define DEV_RANDOM (1 << 29)              /* ufile flags of an F_NULL: /dev/urandom, /dev/random */
#define DEV_ZERO   (1 << 28)              /* ... /dev/zero */               /* ufile flags of /dev/kmsg (an F_TTY that writes to the kernel log) */
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
int fd_alloc(proc_t *p, int from) { return alloc_fd(p, from); }
void path_abs(proc_t *p, int dirfd, const char *in, char *out, usize cap) { abs_path(p, dirfd, in, out, cap); }

/* a new descriptor for a kernel object; the object's reference passes to it */
i64 fd_install_obj(proc_t *p, kobj_t *o, int flags) {
    int fd = alloc_fd(p, 0);
    if (fd < 0) { kobj_put(o); return fd; }
    p->fd[fd] = (ufile_t){ F_OBJ };
    p->fd[fd].obj = o;
    p->fd[fd].flags = 2 | (flags & 04000);                     /* O_RDWR, O_NONBLOCK */
    p->fd[fd].cloexec = (flags & 02000000) != 0;
    return fd;
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
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random") || !strcmp(path, "/dev/zero")) {
        int fd = alloc_fd(p, 0);
        if (fd >= 0) { p->fd[fd] = (ufile_t){ F_NULL }; p->fd[fd].flags = path[5] == 'z' ? DEV_ZERO : DEV_RANDOM; }
        return fd;
    }
    if (!strcmp(path, "/dev/kmsg")) { int fd = alloc_fd(p, 0); if (fd >= 0) { p->fd[fd] = (ufile_t){ F_TTY }; p->fd[fd].flags = KMSG; } return fd; }
    abs_path(p, dirfd, path, full, sizeof full);
    if (!strncmp(full, "/dev/shm/", 9) && full[9] && !strchr(full + 9, '/')) {     /* shm_open */
        i64 err = 0;
        kobj_t *o = shm_named(full + 9, flags & O_CREAT, (flags & 0200) != 0, &err);
        if (!o) return err;
        if (flags & O_TRUNC) shm_truncate(o, 0);
        return fd_install_obj(p, o, flags);
    }
    vnode_t *n = vfs_lookup(full);
    if (!n) {
        if (!(flags & O_CREAT)) return -ENOENT;
        n = vfs_create(full, 0);
    } else if ((flags & O_TRUNC) && !n->dir) vfs_truncate(n);
    if ((flags & O_DIRECTORY) && !n->dir) return -ENOTDIR;
    int fd = alloc_fd(p, 0);
    if (fd < 0) return fd;
    p->fd[fd] = (ufile_t){ n->dir ? F_DIR : F_FILE, n, (flags & O_APPEND) ? vfs_size(n) : 0, flags, 0 };
    p->fd[fd].cloexec = (flags & 02000000) != 0;
    return fd;
}

/* ---- pipes --------------------------------------------------------------------------- */
#define PIPE_SIZE 65536
typedef struct upipe { u8 *buf; u32 head, tail; int readers, writers; } upipe_t;
enum { EPIPE = 32 };

static upipe_t *pipe_new(void) {
    upipe_t *pp = kalloc(sizeof *pp);
    pp->buf = kalloc(PIPE_SIZE);
    pp->readers = pp->writers = 1;
    return pp;
}

static void pipe_unref(upipe_t *pp, int wr) {
    u64 fl = irq_save();
    if (wr) pp->writers--; else pp->readers--;
    int gone = pp->readers <= 0 && pp->writers <= 0;
    irq_restore(fl);
    if (gone) { kfree(pp->buf); kfree(pp); }
}

static i64 pipe_read(proc_t *p, upipe_t *pp, u8 *dst, u64 len, int nonblock) {
    for (;;) {
        u64 fl = irq_save();
        u32 avail = pp->head - pp->tail;
        if (avail) {
            u64 n = MIN(len, avail);
            for (u64 i = 0; i < n; i++) dst[i] = pp->buf[(pp->tail + i) % PIPE_SIZE];
            pp->tail += (u32)n;
            irq_restore(fl);
            return (i64)n;
        }
        int eof = pp->writers <= 0;
        irq_restore(fl);
        if (eof) return 0;
        if (nonblock) return -EAGAIN;
        if (proc_interrupted(p)) return -EINTR;
        thread_sleep_ms(1);
    }
}

static i64 pipe_write(proc_t *p, upipe_t *pp, const u8 *src, u64 len, int nonblock) {
    u64 done = 0;
    while (done < len) {
        u64 fl = irq_save();
        if (pp->readers <= 0) { irq_restore(fl); return done ? (i64)done : -EPIPE; }
        u32 room = PIPE_SIZE - (pp->head - pp->tail);
        u64 n = MIN(len - done, room);
        for (u64 i = 0; i < n; i++) pp->buf[(pp->head + i) % PIPE_SIZE] = src[done + i];
        pp->head += (u32)n;
        irq_restore(fl);
        done += n;
        if (done < len) {
            if (nonblock) return done ? (i64)done : -EAGAIN;
            if (proc_interrupted(p)) return done ? (i64)done : -EINTR;
            thread_sleep_ms(1);
        }
    }
    return (i64)done;
}

/* ---- descriptor references (dup, fork, close, exit, SCM_RIGHTS) ---------------------------- */
void ufile_ref(ufile_t *f) {
    switch (f->type) {
    case F_SOCK: lsock_ref(f->sock, 1); break;
    case F_PIPE: { u64 fl = irq_save(); if (f->flags & 1) f->pipe->writers++; else f->pipe->readers++; irq_restore(fl); break; }
    case F_OBJ: kobj_get(f->obj); break;
    }
}

void ufile_unref(ufile_t *f) {
    switch (f->type) {
    case F_SOCK: lsock_ref(f->sock, -1); break;
    case F_PIPE: pipe_unref(f->pipe, f->flags & 1); break;
    case F_OBJ: kobj_put(f->obj); break;
    }
    f->type = F_NONE;
}

void fd_addref(proc_t *p, int fd) { ufile_ref(&p->fd[fd]); }
void fd_release(proc_t *p, int fd) { ufile_unref(&p->fd[fd]); }

void fds_release_all(proc_t *p) {
    for (int fd = 0; fd < MAX_FDS; fd++) if (p->fd[fd].type) fd_release(p, fd);
}

static i64 do_write(proc_t *p, int fd, u64 buf, u64 len) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    ufile_t *f = &p->fd[fd];
    if (!UOK(buf, len)) return -EFAULT;
    switch (f->type) {
    case F_TTY:
        if (f->flags & KMSG) {                                    /* /dev/kmsg: lines of the kernel log */
            const char *s = (const char *)(usize)buf;
            for (u64 i = 0; i < len;) {
                char line[200];
                usize n = 0;
                while (i < len && s[i] != '\n' && n < sizeof line - 1) { if (s[i] != '\r') line[n++] = s[i]; i++; }
                if (i < len && s[i] == '\n') i++;
                line[n] = 0;
                klog("%s: %s", p->name, line);
            }
            return (i64)len;
        }
        term_append(p->term, (const char *)(usize)buf, len); return (i64)len;
    case F_NULL: return (i64)len;
    case F_FILE: {
        i64 r = vfs_write(f->vn, f->off, (const void *)(usize)buf, len);
        if (r > 0) f->off += (u64)r;
        return r;
    }
    case F_DIR: return -EISDIR;
    case F_SOCK: { i64 r = lsock_sendto(p, fd, buf, len, 0, 0, 0); if (r == -EPIPE) sig_post(p, thread_current(), 13, 0, p->pid, 0); return r; }
    case F_PIPE: {
        if (!(f->flags & 1)) return -EBADF;
        i64 r = pipe_write(p, f->pipe, (const u8 *)(usize)buf, len, f->flags & 04000);
        if (r == -EPIPE) sig_post(p, thread_current(), 13, 0, p->pid, 0);       /* SIGPIPE */
        return r;
    }
    case F_OBJ: return kobj_write(p, f, buf, len);
    default: return -EBADF;
    }
}

static i64 do_read(proc_t *p, int fd, u64 buf, u64 len) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    ufile_t *f = &p->fd[fd];
    if (!UOK(buf, len)) return -EFAULT;
    switch (f->type) {
    case F_NULL:
        if (f->flags & DEV_RANDOM) { random_bytes((u8 *)(usize)buf, (usize)len); return (i64)len; }
        if (f->flags & DEV_ZERO) { memset((void *)(usize)buf, 0, len); return (i64)len; }
        return 0;
    case F_TTY: return 0;                               /* no keyboard for programs yet: EOF */
    case F_FILE: {
        i64 r = vfs_read(f->vn, f->off, (void *)(usize)buf, len);
        if (r > 0) f->off += (u64)r;
        return r;
    }
    case F_DIR: return -EISDIR;
    case F_SOCK: return lsock_recvfrom(p, fd, buf, len, 0, 0, 0);
    case F_PIPE: return (f->flags & 1) ? -EBADF : pipe_read(p, f->pipe, (u8 *)(usize)buf, len, f->flags & 04000);
    case F_OBJ: return kobj_read(p, f, buf, len);
    default: return -EBADF;
    }
}

/* sendfile: the file's bytes from *offset (or the file position) to out_fd, through a kernel
 * buffer; the output's own write path (socket, pipe, file) sees it as an ordinary write.
 * Ladybird's disk cache sends cached responses this way. */
static kobj_t *obj_of(proc_t *p, u64 fd, int kind);
static i64 do_sendfile(proc_t *p, int out, int in, u64 uoff, u64 count) {
    if (in < 0 || in >= MAX_FDS || out < 0 || out >= MAX_FDS || !p->fd[out].type) return -EBADF;
    ufile_t *f = &p->fd[in];
    kobj_t *shm = f->type == F_OBJ ? obj_of(p, (u64)in, KO_SHM) : NULL;
    if (f->type != F_FILE && !shm) return f->type ? -EINVAL : -EBADF;
    if (uoff && !UOK(uoff, 8)) return -EFAULT;
    u64 off = uoff ? *(u64 *)(usize)uoff : f->off;
    u64 cap = MIN(count, 64 * 1024);
    if (!cap) return 0;
    u8 *buf = kalloc(cap);
    i64 done = 0;
    thread_t *me = thread_current();
    while ((u64)done < count) {
        u64 want = MIN(cap, count - (u64)done);
        i64 n = shm ? shm_rw(shm, off, buf, want, 0) : vfs_read(f->vn, off, buf, want);
        if (n <= 0) { if (!done && n < 0) done = n; break; }
        me->kbuf = 1;
        i64 w = do_write(p, out, (u64)(usize)buf, (u64)n);
        me->kbuf = 0;
        if (w <= 0) { if (!done) done = w; break; }
        done += w; off += (u64)w;
        if (w < n) break;                                  /* the socket is full: the caller polls */
    }
    kfree(buf);
    if (done > 0) { if (uoff) *(u64 *)(usize)uoff = off; else f->off = off; }
    return done;
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
    case F_PIPE: {
        upipe_t *pp = p->fd[fd].pipe;
        if (p->fd[fd].flags & 1) { if (pp->readers <= 0) rev |= POLLERR; else if (pp->head - pp->tail < PIPE_SIZE) rev |= events & POLLOUT; }
        else { if (pp->head != pp->tail) rev |= events & POLLIN; if (pp->writers <= 0) rev |= POLLHUP; }
        break;
    }
    case F_OBJ: rev = kobj_poll(p, &p->fd[fd], events); break;
    default: rev = events & (POLLIN | POLLOUT); break;
    }
    return rev;
}
int fd_poll(proc_t *p, int fd, int events) { return fd_ready(p, fd, events); }

static i64 do_poll(proc_t *p, u64 ufds, u64 n, i64 timeout_ms) {
    if (n > 4096 || (n && !UOK(ufds, n * 8))) return -EINVAL;
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
        if (proc_interrupted(p)) return -EINTR;
        thread_sleep_ms(5);
    }
}

static i64 do_select(proc_t *p, int nfds, u64 rfds, u64 wfds, u64 efds, i64 timeout_ms) {
    if (nfds < 0) return -EINVAL;
    if (nfds > MAX_FDS) nfds = MAX_FDS;
    usize bytes = (usize)((nfds + 63) / 64) * 8;
    u64 rin[MAX_FDS / 64] = { 0 }, win[MAX_FDS / 64] = { 0 };
    if (rfds && bytes) { if (!UOK(rfds, bytes)) return -EFAULT; memcpy(rin, (void *)(usize)rfds, bytes); }
    if (wfds && bytes) { if (!UOK(wfds, bytes)) return -EFAULT; memcpy(win, (void *)(usize)wfds, bytes); }
    if (efds && bytes && UOK(efds, bytes)) memset((void *)(usize)efds, 0, bytes);
    u64 end = timeout_ms < 0 ? ~0ull : k_now_ms() + (u64)timeout_ms;
    for (;;) {
        u64 rout[MAX_FDS / 64] = { 0 }, wout[MAX_FDS / 64] = { 0 };
        int count = 0;
        for (int fd = 0; fd < nfds; fd++) {
            int want = ((rin[fd / 64] >> (fd % 64)) & 1 ? POLLIN : 0) | ((win[fd / 64] >> (fd % 64)) & 1 ? POLLOUT : 0);
            if (!want) continue;
            int rev = fd_ready(p, fd, want);
            if (rev & POLLNVAL) return -EBADF;
            if (rev & (POLLIN | POLLHUP)) { rout[fd / 64] |= 1ull << (fd % 64); count++; }
            if (rev & POLLOUT) { wout[fd / 64] |= 1ull << (fd % 64); count++; }
        }
        if (count || k_now_ms() >= end) {
            if (rfds && bytes) memcpy((void *)(usize)rfds, rout, bytes);
            if (wfds && bytes) memcpy((void *)(usize)wfds, wout, bytes);
            return count;
        }
        if (proc_interrupted(p)) return -EINTR;
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

#define MAP_SHARED          0x01
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_32BIT           0x40
#define MAP_FIXED_NOREPLACE 0x100000
#define VMA_FILE            2              /* vma.shared bit: a private copy of a file (MADV_DONTNEED keeps it) */

static int urange(u64 start, u64 end) {
    if (end <= start) return 0;
    if (end <= USER_STACK_TOP - USER_STACK_SIZE) return start >= PAGE;
    return user_high_range(start, end);
}

/*
 * mmap: anonymous memory, private copies of files, and shared memory - MAP_SHARED of
 * a memfd or /dev/shm object maps the object's own pages (every process mapping it
 * sees the same bytes); MAP_SHARED|MAP_ANONYMOUS makes an anonymous object that
 * fork() children share.  Pages appear on first touch with the requested protection.
 */
static i64 do_mmap(proc_t *p, u64 addr, u64 len, u64 prot, u64 flags, i64 fd, u64 off) {
    if (!len || (off & (PAGE - 1))) return -EINVAL;
    if (len > USER_HIGH_END - USER_HOLE_END) return -ENOMEM;       /* the largest region */
    len = (len + PAGE - 1) & ~(PAGE - 1);
    int anon = (flags & MAP_ANONYMOUS) != 0, shared = (flags & 3) == MAP_SHARED || (flags & 3) == 3;
    ufile_t *uf = NULL;
    kobj_t *shm = NULL;
    if (!anon) {
        if (fd < 0 || fd >= MAX_FDS || !p->fd[fd].type) return -EBADF;
        uf = &p->fd[fd];
        if (uf->type == F_OBJ && uf->obj->kind == KO_SHM) shm = uf->obj;
        else if (uf->type != F_FILE) return -19;                 /* ENODEV */
    }
    kobj_t *backing = NULL, *made = NULL;
    u64 boff = 0;
    if (shared && shm) { backing = shm; boff = off; }
    else if (shared && anon) { backing = made = shm_new("anon"); shm_truncate(made, len); }
    u64 va;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (addr & (PAGE - 1)) { kobj_put(made); return -EINVAL; }
        if (!urange(addr, addr + len)) { kobj_put(made); return -ENOMEM; }
        va = addr;
        if ((flags & MAP_FIXED_NOREPLACE) && !(flags & MAP_FIXED))
            if (proc_range_mapped(p, va, va + len)) { kobj_put(made); return -EEXIST; }
        proc_unmap(p, va, va + len);                               /* MAP_FIXED replaces what was there */
    } else {
        u64 hint = addr & ~(PAGE - 1);
        va = hint && proc_range_free(p, hint, hint + len) ? hint : (flags & MAP_32BIT) ? proc_find_free_low(p, len) : proc_find_free(p, len);
        if (!va) { kobj_put(made); return -ENOMEM; }
    }
    int file_copy = !anon && !backing;                             /* a private copy of the contents */
    int e = proc_add_vma_flags(p, va, va + len, (u32)(prot & 7), backing, boff, file_copy ? VMA_FILE : 0);
    kobj_put(made);                                                /* the VMA holds it now */
    if (e) return -ENOMEM;
    if (file_copy) {
        u64 fsz = uf->type == F_FILE ? vfs_size(uf->vn) : shm_size(shm);
        if (off < fsz && (prot & 7)) {
            u64 n = MIN(len, fsz - off);
            if (UOK(va, n)) {
                if (uf->type == F_FILE) vfs_read(uf->vn, off, (void *)(usize)va, n);
                else shm_rw(shm, off, (void *)(usize)va, n, 0);
            }
        }
    }
    return (i64)va;
}

i64 map_shared(proc_t *p, kobj_t *o, u64 len) {
    len = (len + PAGE - 1) & ~(PAGE - 1);
    u64 va = proc_find_free(p, len);
    if (!va) return -ENOMEM;
    if (proc_add_vma_prot(p, va, va + len, PROT_READ | PROT_WRITE, o, 0)) return -ENOMEM;
    return (i64)va;
}

/* mremap: shrink in place, grow in place when the range after is free, else move the pages */
static i64 do_mremap(proc_t *p, u64 old, u64 olen, u64 nlen, u64 flags, u64 naddr) {
    if ((old & (PAGE - 1)) || !nlen || (flags & ~7ull)) return -EINVAL;
    olen = (olen + PAGE - 1) & ~(PAGE - 1);
    nlen = (nlen + PAGE - 1) & ~(PAGE - 1);
    vma_t *v = proc_vma(p, old);
    if (!v || (olen && old + olen > v->end)) return -EFAULT;
    if (!olen) return -EINVAL;                                     /* duplicating shared mappings: not needed yet */
    int fixed = (flags & 2) != 0;
    if (fixed && (!(flags & 1) || (naddr & (PAGE - 1)) || !urange(naddr, naddr + nlen))) return -EINVAL;
    if (!fixed) {
        if (nlen <= olen) { if (nlen < olen) proc_unmap(p, old + nlen, old + olen); return (i64)old; }
        if (old + olen == v->end && proc_range_free(p, old + olen, old + nlen)) { v->end = old + nlen; return (i64)old; }
        if (!(flags & 1)) return -ENOMEM;                          /* MREMAP_MAYMOVE */
    }
    u64 dst = fixed ? naddr : proc_find_free(p, nlen);
    if (!dst) return -ENOMEM;
    if (fixed) proc_unmap(p, dst, dst + nlen);
    v = proc_vma(p, old);
    if (!v) return -EFAULT;
    u32 prot = v->prot, sh = v->shared;
    kobj_t *obj = v->obj;
    u64 off = v->off + (old - v->start);
    if (obj) kobj_get(obj);                                        /* v may go away below */
    if (proc_add_vma_prot(p, dst, dst + nlen, prot, obj, off)) { kobj_put(obj); return -ENOMEM; }
    vma_t *nv = proc_vma(p, dst);
    if (nv) nv->shared = sh;
    for (u64 a = 0; a < MIN(olen, nlen); a += PAGE) as_move(p->cr3, old + a, dst + a);
    proc_unmap(p, old, old + olen);
    kobj_put(obj);
    return (i64)dst;
}

/* MADV_DONTNEED: private anonymous pages read as zero afterwards (jemalloc relies on it) */
static i64 do_madvise(proc_t *p, u64 addr, u64 len, int advice) {
    if (addr & (PAGE - 1)) return -EINVAL;
    if (advice != 4) return 0;
    u64 end = addr + ((len + PAGE - 1) & ~(PAGE - 1));
    for (u64 a = addr; a < end;) {
        vma_t *v = proc_vma(p, a);
        if (!v) { a += PAGE; continue; }
        u64 e = MIN(end, v->end);
        if (!v->shared) as_unmap_range(p->cr3, a, e);
        a = e;
    }
    return 0;
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
        if (proc_interrupted(p)) { r = -EINTR; break; }
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
    if (want < p->brk_start || want >= USER_BRK_MAX) return (i64)p->brk;
    if (proc_range_mapped(p, p->brk, (want + PAGE - 1) & ~(PAGE - 1))) return (i64)p->brk;   /* never into a mapping */
    want = (want + PAGE - 1) & ~(PAGE - 1);
    if (want > p->brk) proc_add_vma(p, p->brk, want);
    p->brk = want;
    return (i64)p->brk;
}

static i64 do_uname(proc_t *p, u64 buf) {
    if (!UOK(buf, 65 * 6)) return -EFAULT;
    char *u = (char *)(usize)buf;
    memset(u, 0, 65 * 6);
    strlcpy(u + 0, p->native ? "QRT" : "Linux", 65);    /* the ABI we speak */
    strlcpy(u + 65, k.is_venue ? "venue" : "qrt", 65);
    strlcpy(u + 130, p->native ? QRT_VERSION : "6.1.0-qrt", 65);   /* (static glibc refuses kernels < 3.2) */
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

/* ---- execve --------------------------------------------------------------------------- */
/* copy a NULL-terminated array of user strings into one kernel buffer */
static int copy_strv(proc_t *p, u64 uv, char **out, int max, char *buf, usize cap, usize *used) {
    int n = 0;
    if (!uv) { out[0] = NULL; return 0; }
    for (;; n++) {
        if (!UOK(uv + (u64)n * 8, 8)) return -EFAULT;
        u64 s1 = ((u64 *)(usize)uv)[n];
        if (!s1) break;
        if (n >= max) return -7;                                /* E2BIG */
        char *dst = buf + *used;
        if (get_path(p, s1, dst, MIN(cap - *used, 4096)) < 0) return -EFAULT;
        out[n] = dst;
        *used += strlen(dst) + 1;
        if (*used + 16 >= cap) return -7;
    }
    out[n] = NULL;
    return n;
}

/* a command QRT does not ship as a file (/bin/ls...) is a busybox applet */
static int applet_path(const char *path, char *out, usize cap) {
    const char *base = strrchr(path, '/');
    if (!base || !vfs_lookup("/bin/busybox")) return 0;
    if (strncmp(path, "/bin/", 5) && strncmp(path, "/usr/bin/", 9) && strncmp(path, "/sbin/", 6) && strncmp(path, "/usr/sbin/", 10)) return 0;
    if (base == path + strlen(path) - 1) return 0;
    strlcpy(out, "/bin/busybox", cap);
    return 1;
}

static i64 exec_common(proc_t *p, frame_t *f, const char *full, u64 uargv, u64 uenvp) {
    char *argv[66], *envp[66];
    usize cap = 32768, used = 0;
    char *buf = kalloc(cap);
    int argc = copy_strv(p, uargv, argv, 64, buf, cap, &used);
    int envc = argc < 0 ? argc : copy_strv(p, uenvp, envp, 64, buf, cap, &used);
    if (argc < 0 || envc < 0) { kfree(buf); return argc < 0 ? argc : envc; }
    if (!argc) { argv[0] = (char *)full; argv[1] = NULL; argc = 1; }
    char path[160];
    strlcpy(path, full, sizeof path);
    if (!vfs_lookup(path) && strcmp(path, "/proc/self/exe") && !applet_path(full, path, sizeof path)) { kfree(buf); return -ENOENT; }
    vnode_t *vn = vfs_lookup(path);
    if (vn && vn->dir) { kfree(buf); return -13; }               /* EACCES */
    i64 r = proc_exec(p, f, path, argv, argc, envp, envc);
    kfree(buf);
    return r;
}

static i64 do_execve(proc_t *p, frame_t *f, u64 upath, u64 uargv, u64 uenvp) {
    char path[160], full[256];
    int e = get_path(p, upath, path, sizeof path);
    if (e) return e;
    abs_path(p, AT_FDCWD, path, full, sizeof full);
    return exec_common(p, f, full, uargv, uenvp);
}

static i64 do_execve_at(proc_t *p, frame_t *f, int dirfd, u64 upath, u64 uargv, u64 uenvp) {
    char path[160], full[256];
    int e = get_path(p, upath, path, sizeof path);
    if (e) return e;
    if (!path[0] && dirfd >= 0 && dirfd < MAX_FDS && p->fd[dirfd].vn) vfs_path(p->fd[dirfd].vn, full, sizeof full);
    else abs_path(p, dirfd, path, full, sizeof full);
    return exec_common(p, f, full, uargv, uenvp);
}

/* a temporary signal mask for the length of a call (ppoll, pselect6, epoll_pwait): the
 * old one comes back afterwards, or after the handler if a signal interrupted the call */
static void mask_push(proc_t *p, u64 umask) {
    thread_t *t = thread_current();
    if (!umask || !UOK(umask, 8)) return;
    t->sig_saved_mask = t->sig_mask;
    t->sig_suspended = 1;
    t->sig_mask = *(u64 *)(usize)umask & ~((1ull << 8) | (1ull << 18));
}
static void mask_pop(i64 r) {
    thread_t *t = thread_current();
    if (t->sig_suspended && r != -EINTR) { t->sig_mask = t->sig_saved_mask; t->sig_suspended = 0; }
}

static ufile_t *fdp(proc_t *p, u64 fd) { return (int)fd >= 0 && (int)fd < MAX_FDS && p->fd[fd].type ? &p->fd[fd] : NULL; }
static int is_unix(proc_t *p, u64 fd) { ufile_t *f = fdp(p, fd); return f && f->type == F_OBJ && f->obj->kind == KO_UNIX; }
static kobj_t *obj_of(proc_t *p, u64 fd, int kind) { ufile_t *f = fdp(p, fd); return f && f->type == F_OBJ && f->obj->kind == kind ? f->obj : NULL; }

static i64 do_unlink(proc_t *p, int dirfd, u64 upath, int dir) {
    char path[160], full[256];
    int e = get_path(p, upath, path, sizeof path);
    if (e) return e;
    abs_path(p, dirfd, path, full, sizeof full);
    if (!strncmp(full, "/dev/shm/", 9) && shm_named_exists(full + 9)) return shm_unlink(full + 9);
    unix_unlink_path(full);
    return vfs_unlink(full, dir);
}

static i64 do_rename(proc_t *p, int dfd1, u64 u1, int dfd2, u64 u2) {
    char a[160], b[160], fa[256], fb[256];
    int e = get_path(p, u1, a, sizeof a);
    if (!e) e = get_path(p, u2, b, sizeof b);
    if (e) return e;
    abs_path(p, dfd1, a, fa, sizeof fa);
    abs_path(p, dfd2, b, fb, sizeof fb);
    return vfs_rename(fa, fb);
}

/* sendmmsg / recvmmsg on Unix sockets: msghdr (56 bytes) + msg_len, 64 bytes apart */
static i64 unix_mmsg(proc_t *p, ufile_t *f, u64 vec, u32 n, int flags, int send) {
    if (n > 1024 || !UOK(vec, (u64)n * 64)) return -EFAULT;
    u32 i = 0;
    for (; i < n; i++) {
        i64 r = send ? unix_sendmsg(p, f, vec + i * 64, flags) : unix_recvmsg(p, f, vec + i * 64, flags | (i ? 0x40 : 0));
        if (r < 0) return i ? (i64)i : r;
        *(u32 *)(usize)(vec + i * 64 + 56) = (u32)r;
    }
    return i;
}

/* ---- dispatch ----------------------------------------------------------------------- */
/* QRT_TRACE=1: a failing call goes to the kernel log, with the path it was given */
static void trace_failure(proc_t *p, u64 nr, u64 a0, u64 a1, i64 r) {
    if (r == -EAGAIN || r == -EINTR) return;
    u64 path = 0;
    switch (nr) {
    case 2: case 4: case 6: case 21: case 59: case 83: case 84: case 87: case 89: case 137: path = a0; break;
    case 257: case 258: case 262: case 263: case 267: case 269: case 332: case 439: path = a1; break;
    }
    char s[80] = "";
    if (path && UOK(path, 1)) {
        usize i = 0;
        for (; i < sizeof s - 1 && UOK(path + i, 1) && ((const char *)(usize)path)[i]; i++) s[i] = ((const char *)(usize)path)[i];
        s[i] = 0;
    }
    klog("trace: %s[%d] sys %llu -> %lld %s", p->name, p->pid, nr, r, s);
}

void syscall_dispatch(frame_t *f) {
    proc_t *p = proc_current();
    u64 nr = f->rax, a0 = f->rdi, a1 = f->rsi, a2 = f->rdx, a3 = f->r10, a4 = f->r8, a5 = f->r9;
    u64 entry_nr = nr;
    i64 r = -ENOSYS;
    thread_t *me = thread_current();
    p->syscalls++;
    me->in_sys = 1;
    /* native QRT programs: QRT's own numbers (sdk/syscalls.txt); the QRT-only calls,
     * and the rest mapped onto the kernel service with the same semantics */
    if (p->native) {
        if (nr >= 1024) {
            r = qrt_call(p, nr, a0, a1, a2, a3, a4);
            nr = ~0ull;
        } else nr = nr <= QRT_NR_LINUX_TOP ? qrt_to_linux[nr] : 0xffff;
    }
    switch (nr) {
    case 0:  r = do_read(p, (int)a0, a1, a2); break;
    case 1:  r = do_write(p, (int)a0, a1, a2); break;
    case 2:  r = do_open(p, AT_FDCWD, a0, (int)a1); break;
    case 257: r = do_open(p, (int)a0, a1, (int)a2); break;
    case 3:
        if ((int)a0 >= 0 && (int)a0 < MAX_FDS && p->fd[a0].type) {
            fd_release(p, (int)a0);
            r = 0;
        } else r = -EBADF;
        break;
    case 221: r = fdp(p, a0) ? 0 : -EBADF; break;       /* fadvise64: a hint */
    case 74: case 75:                                   /* fsync, fdatasync: files live in RAM */
        r = fdp(p, a0) ? 0 : -EBADF; break;
    case 90: case 91: case 92: case 93: case 94: case 260: case 268:   /* chmod, fchmod, chown, fchown, lchown, fchownat, fchmodat */
        r = 0; break;                                   /* one user, no permissions: accepted */
    case 86: case 265: r = -EPERM; break;
    case 73: r = fdp(p, a0) ? 0 : -EBADF; break;       /* flock: granted (lock files of one user's programs) */               /* link, linkat: no hard links (callers fall back) */
    case 137: case 138: {                               /* statfs, fstatfs: a tmpfs */
        u64 buf = a1;
        if (!UOK(buf, 120)) { r = -EFAULT; break; }
        u64 *s = (u64 *)(usize)buf;
        memset(s, 0, 120);
        s[0] = 0x01021994;                              /* f_type: TMPFS_MAGIC */
        s[1] = PAGE;                                    /* f_bsize */
        s[2] = pmm_total_bytes() / PAGE;                /* f_blocks */
        s[3] = s[4] = pmm_free_bytes() / PAGE;          /* f_bfree, f_bavail */
        s[5] = 65536; s[6] = 65536;                     /* f_files, f_ffree */
        s[8] = 255;                                     /* f_namelen */
        s[9] = PAGE;                                    /* f_frsize */
        r = 0; break;
    }
    case 436:                                           /* close_range(first, last, flags): CLOSE_RANGE_CLOEXEC = 4 */
        if (a0 > a1 || (a2 & ~4ull)) { r = -EINVAL; break; }
        for (u64 fd = a0; fd <= a1 && fd < MAX_FDS; fd++) {
            if (!p->fd[fd].type) continue;
            if (a2 & 4) p->fd[fd].cloexec = 1; else fd_release(p, (int)fd);
        }
        r = 0;
        break;
    /* sockets (lsock.c) */
    /* sockets: AF_UNIX here (unix.c), AF_INET over the network stack (lsock.c) */
    case 41: r = a0 == 1 ? unix_socket(p, (int)a1, (int)a1) : lsock_socket(p, (int)a0, (int)a1, (int)a2); break;
    case 53: r = a0 == 1 ? unix_socketpair(p, (int)a1, (int)a1, a3) : -95; break;
    case 42: r = is_unix(p, a0) ? unix_connect(p, fdp(p, a0), a1, a2) : lsock_connect(p, (int)a0, a1, a2); break;
    case 49: r = is_unix(p, a0) ? unix_bind(p, fdp(p, a0), a1, a2) : lsock_bind(p, (int)a0, a1, a2); break;
    case 50: r = is_unix(p, a0) ? unix_listen(p, fdp(p, a0), (int)a1) : -95; break;
    case 43: case 288: r = is_unix(p, a0) ? unix_accept(p, fdp(p, a0), a1, a2, nr == 288 ? (int)a3 : 0) : -95; break;
    case 44: r = is_unix(p, a0) ? unix_sendto(p, fdp(p, a0), a1, a2, (int)a3, a4, a5) : lsock_sendto(p, (int)a0, a1, a2, (int)a3, a4, a5); break;
    case 45: r = is_unix(p, a0) ? unix_recvfrom(p, fdp(p, a0), a1, a2, (int)a3, a4, a5) : lsock_recvfrom(p, (int)a0, a1, a2, (int)a3, a4, a5); break;
    case 46: r = is_unix(p, a0) ? unix_sendmsg(p, fdp(p, a0), a1, (int)a2) : lsock_sendmsg(p, (int)a0, a1, (int)a2); break;
    case 47: r = is_unix(p, a0) ? unix_recvmsg(p, fdp(p, a0), a1, (int)a2) : lsock_recvmsg(p, (int)a0, a1, (int)a2); break;
    case 48: r = is_unix(p, a0) ? unix_shutdown(p, fdp(p, a0), (int)a1) : lsock_shutdown(p, (int)a0, (int)a1); break;
    case 51: case 52: r = is_unix(p, a0) ? unix_getname(p, fdp(p, a0), a1, a2, nr == 52) : lsock_getname(p, (int)a0, a1, a2, nr == 52); break;
    case 54: r = 0; break;                                        /* setsockopt: accepted */
    case 55: r = is_unix(p, a0) ? unix_getsockopt(p, fdp(p, a0), (int)a1, (int)a2, a3, a4) : lsock_getsockopt(p, (int)a0, (int)a1, (int)a2, a3, a4); break;
    case 307: r = is_unix(p, a0) ? unix_mmsg(p, fdp(p, a0), a1, (u32)a2, (int)a3, 1) : lsock_sendmmsg(p, (int)a0, a1, (u32)a2, (int)a3); break;
    case 299: r = is_unix(p, a0) ? unix_mmsg(p, fdp(p, a0), a1, (u32)a2, (int)a3, 0) : -38; break;   /* recvmmsg */
    /* events */
    case 284: case 290: r = fd_install_obj(p, eventfd_new((u32)a0, nr == 290 && (a1 & 1)), nr == 290 ? (int)a1 : 0); break;
    case 283: r = fd_install_obj(p, timerfd_new((int)a0), (int)a1); break;
    case 286: { kobj_t *o = obj_of(p, a0, KO_TIMERFD); r = o ? timerfd_settime(p, o, (int)a1, a2, a3) : -EINVAL; break; }
    case 287: { kobj_t *o = obj_of(p, a0, KO_TIMERFD); r = o ? timerfd_gettime(p, o, a1) : -EINVAL; break; }
    case 282: case 289: {                                         /* signalfd, signalfd4 */
        if (a2 != 8 || !UOK(a1, 8)) { r = -EINVAL; break; }
        u64 m = *(u64 *)(usize)a1;
        if ((i32)a0 == -1) { r = fd_install_obj(p, signalfd_new(m), nr == 289 ? (int)a3 : 0); break; }
        kobj_t *o = obj_of(p, a0, KO_SIGNALFD);
        if (o) { signalfd_set(o, m); r = (i64)(i32)a0; } else r = -EINVAL;
        break;
    }
    case 213: case 291: r = fd_install_obj(p, epoll_new(), nr == 291 ? (int)a0 : 0); break;
    case 233: { kobj_t *o = obj_of(p, a0, KO_EPOLL); r = o ? epoll_ctl(p, o, (int)a1, (int)a2, a3) : -EINVAL; break; }
    case 232: case 281: case 441: {                               /* epoll_wait, epoll_pwait, epoll_pwait2 */
        kobj_t *o = obj_of(p, a0, KO_EPOLL);
        if (!o) { r = -EINVAL; break; }
        i64 ms = (i64)(i32)a3;
        if (nr == 441) { ms = -1; if (a3) { if (!UOK(a3, 16)) { r = -EFAULT; break; } u64 *ts = (u64 *)(usize)a3; ms = (i64)(ts[0] * 1000 + ts[1] / 1000000); } }
        if (nr != 232) mask_push(p, a4);
        r = epoll_wait(p, o, a1, (int)a2, ms);
        if (nr != 232) mask_pop(r);
        break;
    }
    case 319: {                                                   /* memfd_create */
        char name[64];
        if (get_path(p, a0, name, sizeof name)) { r = -EFAULT; break; }
        r = fd_install_obj(p, shm_new(name), (a1 & 1) ? 02000000 : 0);
        break;
    }
    /* signals (signal.c) */
    case 13: r = sig_action(p, (int)a0, a1, a2, a3); break;
    case 14: r = sig_procmask(p, (int)a0, a1, a2, a3); break;
    case 15: r = sig_return(p, f); break;
    case 131: r = sig_altstack(p, a0, a1); break;
    case 127: r = sig_pending_set(p, a0, a1); break;
    case 128: r = sig_timedwait(p, a0, a1, a2, a3); break;
    case 130: r = sig_suspend(p, a0, a1); break;
    case 34: r = sig_suspend(p, 0, 8); break;                     /* pause */
    case 36: r = sig_getitimer(p, (int)a0, a1); break;
    case 38: r = sig_setitimer(p, (int)a0, a1, a2); break;
    case 200: r = sig_kill_thread(p, 0, (int)a0, (int)a1); break;        /* tkill */
    case 234: r = sig_kill_thread(p, (int)a0, (int)a1, (int)a2); break;  /* tgkill */
    case 129: r = proc_signal(p, (int)a0, (int)a1); break;               /* rt_sigqueueinfo */
    /* files */
    case 87: r = do_unlink(p, AT_FDCWD, a0, 0); break;
    case 84: r = do_unlink(p, AT_FDCWD, a0, 1); break;            /* rmdir */
    case 263: r = do_unlink(p, (int)a0, a1, (a2 & 0x200) != 0); break;
    case 82: r = do_rename(p, AT_FDCWD, a0, AT_FDCWD, a1); break;
    case 264: case 316: r = do_rename(p, (int)a0, a1, (int)a2, a3); break;
    case 7: r = do_poll(p, a0, a1, (i64)(i32)a2); break;
    case 37: r = sig_alarm(p, a0); break;
    case 77:                                                      /* ftruncate */
        if (obj_of(p, a0, KO_SHM)) { r = shm_truncate(p->fd[a0].obj, a1); break; }
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || p->fd[a0].type != F_FILE) { r = -EBADF; break; }
        if (a1 <= p->fd[a0].vn->size) p->fd[a0].vn->size = a1;
        else { static const u8 z[512]; u64 at = p->fd[a0].vn->size; while (at < a1) { u64 n = MIN(a1 - at, (u64)sizeof z); vfs_write(p->fd[a0].vn, at, z, n); at += n; } }
        r = 0;
        break;
    case 271: {                                                   /* ppoll: timespec, temporary mask */
        i64 ms = -1;
        if (a2) { if (!UOK(a2, 16)) { r = -EFAULT; break; } u64 *ts = (u64 *)(usize)a2; ms = (i64)(ts[0] * 1000 + ts[1] / 1000000); }
        mask_push(p, a3);
        r = do_poll(p, a0, a1, ms);
        mask_pop(r);
        break;
    }
    case 23: case 270: {                                          /* select (timeval), pselect6 (timespec) */
        i64 ms = -1;
        if (a4) {
            if (!UOK(a4, 16)) { r = -EFAULT; break; }
            u64 *t = (u64 *)(usize)a4;
            ms = nr == 23 ? (i64)(t[0] * 1000 + t[1] / 1000) : (i64)(t[0] * 1000 + t[1] / 1000000);
        }
        if (nr == 270 && a5 && UOK(a5, 16)) mask_push(p, *(u64 *)(usize)a5);
        r = do_select(p, (int)a0, a1, a2, a3, ms);
        if (nr == 270) mask_pop(r);
        break;
    }
    case 4:  r = do_stat_path(p, AT_FDCWD, a0, a1, 0); break;
    case 6:  r = do_stat_path(p, AT_FDCWD, a0, a1, AT_SYMLINK_NOFOLLOW); break;
    case 262: r = do_stat_path(p, (int)a0, a1, a2, (int)a3); break;
    case 5:
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || !p->fd[a0].type) { r = -EBADF; break; }
        if (!UOK(a1, sizeof(lstat_t))) { r = -EFAULT; break; }
        fill_stat((lstat_t *)(usize)a1, p->fd[a0].vn, p->fd[a0].type == F_TTY);
        if (p->fd[a0].type == F_PIPE) ((lstat_t *)(usize)a1)->st_mode = 0010600;
        if (p->fd[a0].type == F_OBJ) {
            lstat_t *st = (lstat_t *)(usize)a1;
            kobj_t *o = p->fd[a0].obj;
            st->st_ino = (u64)(usize)o / 16;
            st->st_mode = o->kind == KO_SHM ? 0100600 : o->kind == KO_UNIX ? 0140777 : 0600;
            st->st_size = o->kind == KO_SHM ? (i64)shm_size(o) : 0;
            st->st_blocks = (st->st_size + 511) / 512;
        }
        r = 0; break;
    case 8: {                                           /* lseek */
        ufile_t *uf = fdp(p, a0);
        int shm = uf && uf->type == F_OBJ && uf->obj->kind == KO_SHM;
        if (!uf || (uf->type != F_FILE && !shm)) { r = uf ? -ESPIPE : -EBADF; break; }
        i64 base = a2 == 0 ? 0 : a2 == 1 ? (i64)uf->off : shm ? (i64)shm_size(uf->obj) : (i64)vfs_size(uf->vn);
        i64 no = base + (i64)a1;
        if (no < 0) { r = -EINVAL; break; }
        uf->off = (u64)no; r = no; break;
    }
    case 17: case 18: {                                 /* pread64, pwrite64 */
        if (!UOK(a1, a2)) { r = -EFAULT; break; }
        if (obj_of(p, a0, KO_SHM)) { r = shm_rw(p->fd[a0].obj, a3, (void *)(usize)a1, a2, nr == 18); break; }
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || p->fd[a0].type != F_FILE) { r = fdp(p, a0) ? -ESPIPE : -EBADF; break; }
        r = nr == 17 ? vfs_read(p->fd[a0].vn, a3, (void *)(usize)a1, a2) : vfs_write(p->fd[a0].vn, a3, (const void *)(usize)a1, a2);
        break;
    }
    case 295: case 296: case 327: case 328: {          /* preadv, pwritev, preadv2, pwritev2 (flags ignored) */
        int wr = nr == 296 || nr == 328;
        if (nr >= 327 && (i64)a3 == -1) { nr = wr ? 20 : 19; goto vec; }       /* the current offset: readv/writev */
        if (!UOK(a1, a2 * 16)) { r = -EFAULT; break; }
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || p->fd[a0].type != F_FILE) { r = fdp(p, a0) ? -ESPIPE : -EBADF; break; }
        const u64 *iov = (const u64 *)(usize)a1;
        u64 off = a3;
        r = 0;
        for (u64 i = 0; i < a2; i++) {
            if (!UOK(iov[i * 2], iov[i * 2 + 1])) { if (!r) r = -EFAULT; break; }
            i64 n = wr ? vfs_write(p->fd[a0].vn, off, (const void *)(usize)iov[i * 2], iov[i * 2 + 1])
                       : vfs_read(p->fd[a0].vn, off, (void *)(usize)iov[i * 2], iov[i * 2 + 1]);
            if (n < 0) { if (!r) r = n; break; }
            r += n; off += (u64)n;
            if ((u64)n < iov[i * 2 + 1]) break;
        }
        break;
    }
    vec:
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
    case 25: r = do_mremap(p, a0, a1, a2, a3, a4); break;
    case 10:                                            /* mprotect */
        if (a0 & (PAGE - 1)) { r = -EINVAL; break; }
        r = a1 ? proc_protect(p, a0, a0 + ((a1 + PAGE - 1) & ~(PAGE - 1)), (u32)(a2 & 7)) : 0;
        break;
    case 28: r = do_madvise(p, a0, a1, (int)a2); break;
    case 26: case 149: case 150: case 151: case 152: case 325: r = 0; break;   /* msync, mlock family: accepted */
    case 27: {                                          /* mincore: everything resident */
        u64 pages = (a1 + PAGE - 1) / PAGE;
        if (!UOK(a2, pages)) { r = -EFAULT; break; }
        memset((void *)(usize)a2, 1, pages);
        r = 0; break;
    }
    case 12: r = do_brk(p, a0); break;
    case 158:                                           /* arch_prctl */
        if (a0 == 0x1002) { thread_current()->fs_base = a1; wrmsr(MSR_FS_BASE, a1); r = 0; }
        else if (a0 == 0x1003) { if (UOK(a1, 8)) { *(u64 *)(usize)a1 = thread_current()->fs_base; r = 0; } else r = -EFAULT; }
        else r = -EINVAL;
        break;
    case 218: me->clear_tid = a0; r = me->tid; break;   /* set_tid_address */
    case 273: r = 0; break;                             /* set_robust_list: accepted */
    case 334: r = -ENOSYS; break;                       /* rseq: glibc copes */
    case 16:                                            /* ioctl */
        if (fdp(p, a0) && p->fd[a0].type == F_TTY && !(p->fd[a0].flags & KMSG) && (a1 == 0x5401 || a1 == 0x5413)) {
            /* the Terminal is a terminal: TCGETS (so isatty() is true and stdout is line-buffered), TIOCGWINSZ */
            if (a1 == 0x5401 && UOK(a2, 60)) { memset((void *)(usize)a2, 0, 60); ((u32 *)(usize)a2)[1] = 5; ((u32 *)(usize)a2)[3] = 0x8a3b; r = 0; }
            else if (a1 == 0x5413 && UOK(a2, 8)) { u16 *ws = (u16 *)(usize)a2; ws[0] = 40; ws[1] = 100; ws[2] = ws[3] = 0; r = 0; }
            else r = -EFAULT;
        } else if (fdp(p, a0) && p->fd[a0].type == F_OBJ) {
            if (a1 == 0x541b && UOK(a2, 4)) { *(i32 *)(usize)a2 = is_unix(p, a0) ? (i32)unix_available(&p->fd[a0]) : 0; r = 0; }
            else if (a1 == 0x5421 && UOK(a2, 4)) { p->fd[a0].flags = (p->fd[a0].flags & ~04000) | (*(i32 *)(usize)a2 ? 04000 : 0); r = 0; }
            else if (a1 == 0x5451 || a1 == 0x5450) { p->fd[a0].cloexec = a1 == 0x5451; r = 0; }          /* FIOCLEX, FIONCLEX */
            else r = -ENOTTY;
        } else if ((int)a0 >= 0 && (int)a0 < MAX_FDS && p->fd[a0].type == F_SOCK) {
            if (a1 == 0x541b && UOK(a2, 4)) { *(i32 *)(usize)a2 = (i32)lsock_available(p, (int)a0); r = 0; }      /* FIONREAD */
            else if (a1 == 0x5421 && UOK(a2, 4)) { lsock_set_nonblock(p, (int)a0, *(i32 *)(usize)a2 != 0); r = 0; } /* FIONBIO */
            else r = -ENOTTY;
        } else r = -ENOTTY;                             /* no terminal control */
        break;
    case 72:                                            /* fcntl */
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || !p->fd[a0].type) { r = -EBADF; break; }
        if (a1 == 0 || a1 == 1030) { int fd = alloc_fd(p, (int)a2); if (fd >= 0) { p->fd[fd] = p->fd[a0]; fd_addref(p, fd); p->fd[fd].cloexec = a1 == 1030; } r = fd; }
        else if (a1 == 1) r = p->fd[a0].cloexec;           /* F_GETFD */
        else if (a1 == 1033 || a1 == 1034) r = 0;          /* F_ADD_SEALS, F_GET_SEALS: no seals are enforced */
        else if (a1 == 2) { p->fd[a0].cloexec = (int)(a2 & 1); r = 0; }   /* F_SETFD */
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
        if (fd >= 0 && (int)a0 >= 0 && (int)a0 < MAX_FDS && p->fd[a0].type) { p->fd[fd] = p->fd[a0]; fd_addref(p, fd); p->fd[fd].cloexec = 0; }
        else if (fd >= 0) fd = -EBADF;
        r = fd;
        break;
    }
    case 33: case 292:
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || (int)a1 < 0 || (int)a1 >= MAX_FDS) { r = -EBADF; break; }
        if (!p->fd[a0].type) { r = -EBADF; break; }
        if (a0 != a1) {
            if (p->fd[a1].type) fd_release(p, (int)a1);
            p->fd[a1] = p->fd[a0];
            fd_addref(p, (int)a1);
            p->fd[a1].cloexec = nr == 292 && (a2 & 02000000);
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
    case 40: r = do_sendfile(p, (int)a0, (int)a1, a2, a3); break;     /* sendfile */
    case 35: case 230: {                                        /* nanosleep, clock_nanosleep */
        u64 ts = nr == 35 ? a0 : a2;
        if (!UOK(ts, 16)) { r = -EFAULT; break; }
        u64 *t = (u64 *)(usize)ts;
        u64 end = k_now_ms() + t[0] * 1000 + t[1] / 1000000;
        r = 0;
        while (k_now_ms() < end) {                              /* in steps, so Stop works */
            if (proc_interrupted(p)) { r = -EINTR; break; }
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
    case 57: r = proc_clone(p, f, 17, 0, 0, 0, 0); break;       /* fork */
    case 58: r = proc_clone(p, f, 0x4000 | 17, 0, 0, 0, 0); break;   /* vfork */
    case 59: r = do_execve(p, f, a0, a1, a2); break;
    case 322: r = do_execve_at(p, f, (int)a0, a1, a2, a3); break;   /* execveat */
    case 61: r = proc_wait(p, (int)a0, a1, (int)a2); break;    /* wait4 */
    case 22: case 293: {                                        /* pipe, pipe2 */
        if (!UOK(a0, 8)) { r = -EFAULT; break; }
        int rfd = alloc_fd(p, 0);
        if (rfd < 0) { r = rfd; break; }
        p->fd[rfd].type = F_PIPE;                               /* reserve it */
        int wfd = alloc_fd(p, 0);
        if (wfd < 0) { p->fd[rfd].type = F_NONE; r = wfd; break; }
        upipe_t *pp = pipe_new();
        int fl = nr == 293 ? (int)a1 : 0;
        p->fd[rfd] = (ufile_t){ F_PIPE, NULL, 0, (fl & 04000), 0 };
        p->fd[wfd] = (ufile_t){ F_PIPE, NULL, 0, (fl & 04000) | 1, 0 };
        p->fd[rfd].pipe = p->fd[wfd].pipe = pp;
        p->fd[rfd].cloexec = p->fd[wfd].cloexec = (fl & 02000000) != 0;
        ((i32 *)(usize)a0)[0] = rfd;
        ((i32 *)(usize)a0)[1] = wfd;
        r = 0; break;
    }
    case 62: r = proc_signal(p, (int)a0, (int)a1); break;      /* kill */
    case 110: r = p->ppid; break;                               /* getppid */
    case 109: case 112: r = nr == 112 ? p->pid : 0; break;      /* setpgid, setsid */
    case 111: case 121: case 124: r = p->pid; break;            /* getpgrp, getpgid, getsid */
    case 60: me->in_sys = 0; proc_thread_exit((int)(a0 & 0xff)); break;   /* exit: this thread */
    case 231: me->in_sys = 0; proc_exit((int)(a0 & 0xff)); break;         /* exit_group */
    case ~0ull: break;                                          /* a QRT-only call, already done */
    default: log_unknown(nr == 0xffff ? entry_nr | 0x10000 : nr); break;   /* 0x1xxxx: a native number */
    }
    f->rax = (u64)r;
    if (p->trace && r < 0 && r > -4096) trace_failure(p, nr, a0, a1, r);
    me->in_sys = 0;
    if (p->killed) proc_thread_exit(137);                       /* the process is going (Stop, exit_group) */
    sig_deliver_pending(p, f, nr, entry_nr, &r);                /* a handler to run (or a call to restart) */
}
