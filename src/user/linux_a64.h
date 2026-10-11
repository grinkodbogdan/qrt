/*
 * linux_a64.h - Linux's AArch64 system-call ABI on top of the x86-64 one linux.c
 * implements (included by linux.c on 64-bit ARM).
 *
 * The number comes in x8, the arguments in x0-x5, the result goes back in x0.  The
 * numbers are asm-generic's (a64_nr.h maps them by name); where the two ABIs' structures
 * or flags differ the arguments are converted on the way in and the results on the way
 * out: struct stat (AArch64 has the generic layout), struct epoll_event (not packed on
 * AArch64), the O_ flags that differ (O_DIRECTORY, O_NOFOLLOW, O_DIRECT, O_LARGEFILE),
 * and clone's argument order (tls before child_tid).  QRT's own calls (windows, input:
 * 1024 and up, sdk/syscalls.txt) have the same numbers as for x86-64's native programs.
 */
#include "a64_nr.h"

typedef struct {
    u64 st_dev, st_ino;
    u32 st_mode, st_nlink, st_uid, st_gid;
    u64 st_rdev, pad1;
    i64 st_size;
    i32 st_blksize, pad2;
    i64 st_blocks;
    u64 st_atime, st_atime_ns, st_mtime, st_mtime_ns, st_ctime, st_ctime_ns;
    u32 unused[2];
} a64_stat_t;
_Static_assert(sizeof(a64_stat_t) == 128, "a64 stat");

typedef struct {
    u64 nr;                  /* the AArch64 number */
    u64 user;                /* the program's buffer a kernel one stands in for */
    void *kb;                /* the kernel's buffer */
    int max;                 /* epoll_pwait: events the program has room for */
} a64_call_t;

/* O_ flags: AArch64 -> x86-64 and back */
static u64 a64_oflags_in(u64 f) {
    u64 o = f & ~0740000ull;
    if (f & 040000) o |= 0200000;    /* O_DIRECTORY */
    if (f & 0100000) o |= 0400000;   /* O_NOFOLLOW */
    if (f & 0200000) o |= 040000;    /* O_DIRECT */
    if (f & 0400000) o |= 0100000;   /* O_LARGEFILE */
    return o;
}
static u64 a64_oflags_out(u64 f) {
    u64 o = f & ~0740000ull;
    if (f & 0200000) o |= 040000;
    if (f & 0400000) o |= 0100000;
    if (f & 040000) o |= 0200000;
    if (f & 0100000) o |= 0400000;
    return o;
}

/* the call as linux.c knows it: returns the x86-64 number (0x10000 | nr: none) */
static u64 a64_in(proc_t *p, a64_call_t *c, u64 nr, u64 *a) {
    thread_t *me = thread_current();
    c->nr = nr; c->user = 0; c->kb = NULL;
    u64 x = nr < ARRAY_LEN(a64_nr) && a64_nr[nr] != 0xffff ? a64_nr[nr] : 0x10000 | nr;
    if (nr >= 1024 && nr < 1040) return nr;                              /* QRT's own (sdk/syscalls.txt) */
    switch (nr) {
    case 56: a[2] = a64_oflags_in(a[2]); break;                          /* openat */
    case 25: if (a[1] == 4) a[2] = a64_oflags_in(a[2]); break;           /* fcntl F_SETFL */
    case 220: { u64 t = a[3]; a[3] = a[4]; a[4] = t; break; }            /* clone: ..., tls, ctid -> ..., ctid, tls */
    case 79: case 80: {                                                  /* newfstatat, fstat */
        int i = nr == 79 ? 2 : 1;
        if (!proc_user_ok(p, a[i], sizeof(a64_stat_t))) return x;        /* linux.c says EFAULT */
        c->user = a[i]; c->kb = kalloc(sizeof(lstat_t));
        a[i] = (u64)(usize)c->kb;
        me->kbuf = 1;
        break;
    }
    case 21:                                                             /* epoll_ctl */
        if (a[3]) {
            if (!proc_user_ok(p, a[3], 16)) { a[3] = 1; break; }         /* a bad pointer: EFAULT */
            u8 *k = kalloc(16);
            memcpy(k, (void *)(usize)a[3], 4);                           /* events */
            memcpy(k + 4, (void *)(usize)(a[3] + 8), 8);                 /* data */
            c->kb = k;
            a[3] = (u64)(usize)k;
            me->kbuf = 1;
        }
        break;
    case 22: {                                                           /* epoll_pwait */
        int max = (int)a[2];
        if (max <= 0 || max > 4096 || !proc_user_ok(p, a[1], (u64)max * 16)) break;
        c->user = a[1]; c->max = max;
        c->kb = kalloc((usize)max * 12);
        a[1] = (u64)(usize)c->kb;
        me->kbuf = 1;
        break;
    }
    }
    return x;
}

/* the result as the program expects it */
static i64 a64_out(proc_t *p, a64_call_t *c, u64 *a, i64 r) {
    (void)p;
    thread_t *me = thread_current();
    me->kbuf = 0;
    switch (c->nr) {
    case 25: if (a[1] == 3 && r >= 0) r = (i64)a64_oflags_out((u64)r); break;   /* F_GETFL */
    case 79: case 80:
        if (c->kb && r == 0) {
            const lstat_t *k = c->kb;
            a64_stat_t s = { k->st_dev, k->st_ino, k->st_mode, (u32)k->st_nlink, k->st_uid, k->st_gid, k->st_rdev, 0,
                             k->st_size, (i32)k->st_blksize, 0, k->st_blocks, k->st_atime, k->st_atime_ns,
                             k->st_mtime, k->st_mtime_ns, k->st_ctime, k->st_ctime_ns, { 0, 0 } };
            memcpy((void *)(usize)c->user, &s, sizeof s);
        }
        break;
    case 22:
        if (c->kb && r > 0) {
            const u8 *k = c->kb;
            for (i64 i = 0; i < r && i < c->max; i++) {
                u8 *u = (u8 *)(usize)(c->user + (u64)i * 16);
                memcpy(u, k + i * 12, 4);
                memset(u + 4, 0, 4);
                memcpy(u + 8, k + i * 12 + 4, 8);
            }
        }
        break;
    }
    if (c->kb) kfree(c->kb);
    return r;
}
