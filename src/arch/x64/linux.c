/*
 * linux.c - the Linux x86-64 system call ABI on top of the Tessera kernel.
 *
 * Enough of Linux for unmodified static binaries (glibc, musl, busybox):
 * files and directories on the VFS, anonymous memory (brk/mmap), TLS
 * (arch_prctl), time, identity, and exit.  Unknown calls return -ENOSYS and
 * are logged once, so missing pieces are easy to find.  Like WSL1 or
 * FreeBSD's Linuxulator, this is the ABI, not the Linux kernel.
 */
#include "proc.h"
#include "mm.h"

enum { EPERM = 1, ENOENT = 2, EBADF = 9, ECHILD = 10, ENOMEM = 12, EFAULT = 14, EEXIST = 17,
       ENOTDIR = 20, EISDIR = 21, EINVAL = 22, EMFILE = 24, ENOTTY = 25, ESPIPE = 29, ERANGE = 34,
       ENOSYS = 38, ENOTEMPTY = 39 };

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
    default: return -EBADF;
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

static i64 do_mmap(proc_t *p, u64 addr, u64 len, u64 prot, u64 flags, i64 fd, u64 off) {
    (void)prot; (void)addr;
    if (!len) return -EINVAL;
    len = (len + PAGE - 1) & ~(PAGE - 1);
    u64 va = p->mmap_next;
    if (va + len > USER_MMAP_END) return -ENOMEM;
    p->mmap_next += len + PAGE;                            /* guard gap */
    if (proc_add_vma(p, va, va + len)) return -ENOMEM;
    if (!(flags & 0x20) && fd >= 0 && fd < MAX_FDS && p->fd[fd].type == F_FILE) {   /* file mapping: private copy */
        if (!UOK(va, len)) return -ENOMEM;
        vfs_read(p->fd[fd].vn, off, (void *)(usize)va, len);
    }
    return (i64)va;
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
    p->syscalls++;
    switch (nr) {
    case 0:  r = do_read(p, (int)a0, a1, a2); break;
    case 1:  r = do_write(p, (int)a0, a1, a2); break;
    case 2:  r = do_open(p, AT_FDCWD, a0, (int)a1); break;
    case 257: r = do_open(p, (int)a0, a1, (int)a2); break;
    case 3:  if ((int)a0 >= 0 && (int)a0 < MAX_FDS && p->fd[a0].type) { p->fd[a0].type = F_NONE; r = 0; } else r = -EBADF; break;
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
    case 11: {                                          /* munmap */
        for (u64 a = a0 & ~(PAGE - 1); a < a0 + a1 && a < USER_TOP; a += PAGE) as_unmap(p->cr3, a);
        r = 0; break;
    }
    case 10: case 28: r = 0; break;                     /* mprotect, madvise: accepted */
    case 12: r = do_brk(p, a0); break;
    case 158:                                           /* arch_prctl */
        if (a0 == 0x1002) { thread_current()->fs_base = a1; wrmsr(MSR_FS_BASE, a1); r = 0; }
        else if (a0 == 0x1003) { if (UOK(a1, 8)) { *(u64 *)(usize)a1 = thread_current()->fs_base; r = 0; } else r = -EFAULT; }
        else r = -EINVAL;
        break;
    case 218: r = p->pid; break;                        /* set_tid_address */
    case 273: case 13: case 14: case 131: r = 0; break; /* robust list, signals, sigaltstack: accepted */
    case 334: r = -ENOSYS; break;                       /* rseq: glibc copes */
    case 16: r = -ENOTTY; break;                        /* ioctl: no terminal control */
    case 72:                                            /* fcntl */
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || !p->fd[a0].type) { r = -EBADF; break; }
        if (a1 == 0 || a1 == 1030) { int fd = alloc_fd(p, (int)a2); if (fd >= 0) p->fd[fd] = p->fd[a0]; r = fd; }
        else if (a1 == 3) r = p->fd[a0].flags & O_ACCMODE ? p->fd[a0].flags : 2;
        else r = 0;
        break;
    case 32: { int fd = alloc_fd(p, 0); if (fd >= 0 && (int)a0 >= 0 && (int)a0 < MAX_FDS) p->fd[fd] = p->fd[a0]; r = fd; break; }
    case 33: case 292:
        if ((int)a0 < 0 || (int)a0 >= MAX_FDS || (int)a1 < 0 || (int)a1 >= MAX_FDS) { r = -EBADF; break; }
        p->fd[a1] = p->fd[a0]; r = (i64)a1; break;
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
    case 186: r = p->pid; break;
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
        thread_sleep_ms(t[0] * 1000 + t[1] / 1000000);
        r = 0; break;
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
        for (u64 i = 0; i < a1; i++) ((u8 *)(usize)a0)[i] = (u8)(rand32() ^ rdtsc64());
        r = (i64)a1; break;
    case 202: r = 0; break;                                     /* futex: single-threaded programs */
    case 56: case 57: case 58: case 59: case 61: r = -ENOSYS; break;   /* clone/fork/vfork/execve/wait4 */
    case 60: case 231: proc_exit((int)(a0 & 0xff)); break;      /* exit, exit_group */
    default: log_unknown(nr); break;
    }
    f->rax = (u64)r;
}
