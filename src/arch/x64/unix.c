/*
 * unix.c - AF_UNIX sockets: what Wayland, D-Bus and Firefox's own IPC (a
 * socketpair between the parent and each content process) talk over.
 *
 * SOCK_STREAM, SOCK_DGRAM and SOCK_SEQPACKET; socketpair; bind to a path or an
 * abstract name; listen/accept/connect; send/recv/sendmsg/recvmsg with
 * SCM_RIGHTS (open files travel between processes, holding a reference while in
 * flight); SO_PEERCRED; shutdown; poll/epoll readiness.  Data is a queue of
 * messages on the receiving socket: datagram and seqpacket reads take one message,
 * stream reads run across messages but stop at one that carries descriptors, as
 * Linux does, so the descriptors arrive with the bytes they were sent with.
 */
#include "lfile.h"
#include "mm.h"

enum { EPERM = 1, ENOENT = 2, EINTR = 4, EBADF = 9, EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EINVAL = 22, EMFILE = 24,
       EPIPE = 32, ENOTSOCK = 88, EMSGSIZE = 90, EPROTOTYPE = 91, EOPNOTSUPP = 95, EADDRINUSE = 98, EISCONN = 106,
       ENOTCONN = 107, ECONNREFUSED = 111, ECONNRESET = 104 };
#define O_NONBLOCK   04000
#define O_CLOEXEC    02000000
#define MSG_PEEK     0x2
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL  0x100
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000
#define MSG_CTRUNC   0x8
enum { POLLIN = 1, POLLOUT = 4, POLLERR = 8, POLLHUP = 0x10, POLLRDHUP = 0x2000 };
enum { ST_STREAM = 1, ST_DGRAM = 2, ST_SEQPACKET = 5 };
#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))
#define RCV_LIMIT (1u << 20)                 /* bytes queued at a receiver before senders wait */
#define MAX_FDS_MSG 253                      /* SCM_MAX_FD */

typedef struct umsg {
    struct umsg *next;
    u32 len, off;
    int nfds;
    ufile_t *fds;
    u8 fromlen;
    char from[108];
    u8 data[];
} umsg_t;

typedef struct usock {
    kobj_t h;
    int type;
    int listening, connected, peer_closed, shut_rd, shut_wr;
    char name[108]; u8 namelen;                  /* bound name (sun_path bytes); namelen 0 = unbound */
    struct usock *peer;                          /* connected stream/seqpacket peer (no reference: each clears the other on release) */
    char dname[108]; u8 dnamelen;                /* a connected datagram socket's destination, looked up per send */
    umsg_t *head, *tail;
    u32 rxbytes;
    struct usock **backlog; int nback, maxback;  /* listening: connections waiting for accept() */
    int pid;                                     /* the creator, for SO_PEERCRED */
} usock_t;

#define MAX_BOUND 128
static usock_t *bound[MAX_BOUND];

static usock_t *us_of(ufile_t *f) { return f->type == F_OBJ && f->obj && f->obj->kind == KO_UNIX ? (usock_t *)f->obj : NULL; }

static usock_t *us_new(proc_t *p, int type) {
    usock_t *s = kalloc(sizeof *s);
    s->h.kind = KO_UNIX;
    s->h.refs = 1;
    s->type = type;
    s->pid = p ? p->pid : 0;
    return s;
}

static void msg_free(umsg_t *m) {
    for (int i = 0; i < m->nfds; i++) ufile_unref(&m->fds[i]);   /* descriptors nobody received */
    if (m->fds) kfree(m->fds);
    kfree(m);
}

void unix_release(kobj_t *o) {
    usock_t *s = (usock_t *)o;
    u64 fl = irq_save();
    if (s->peer) { s->peer->peer = NULL; s->peer->peer_closed = 1; s->peer = NULL; }
    for (int i = 0; i < MAX_BOUND; i++) if (bound[i] == s) bound[i] = NULL;
    irq_restore(fl);
    for (umsg_t *m = s->head, *n; m; m = n) { n = m->next; msg_free(m); }
    for (int i = 0; i < s->nback; i++) kobj_put(&s->backlog[i]->h);
    if (s->backlog) kfree(s->backlog);
    kfree(s);
}

static int type_of(int type) { return type & 0xf; }

i64 unix_socket(proc_t *p, int type, int flags) {
    int t = type_of(type);
    if (t != ST_STREAM && t != ST_DGRAM && t != ST_SEQPACKET) return -EPROTOTYPE;
    usock_t *s = us_new(p, t);
    return fd_install_obj(p, &s->h, flags);
}

i64 unix_socketpair(proc_t *p, int type, int flags, u64 sv) {
    int t = type_of(type);
    if (t != ST_STREAM && t != ST_DGRAM && t != ST_SEQPACKET) return -EPROTOTYPE;
    if (!UOK(sv, 8)) return -EFAULT;
    usock_t *a = us_new(p, t), *b = us_new(p, t);
    a->peer = b; b->peer = a;
    a->connected = b->connected = 1;
    i64 fa = fd_install_obj(p, &a->h, flags);
    if (fa < 0) { kobj_put(&a->h); kobj_put(&b->h); return fa; }
    i64 fb = fd_install_obj(p, &b->h, flags);
    if (fb < 0) { ufile_unref(&p->fd[fa]); p->fd[fa].type = F_NONE; kobj_put(&b->h); return fb; }
    ((i32 *)(usize)sv)[0] = (i32)fa;
    ((i32 *)(usize)sv)[1] = (i32)fb;
    return 0;
}

/* sockaddr_un -> the name bytes (filesystem paths made absolute); 0 if malformed */
static int parse_addr(proc_t *p, u64 addr, u64 len, char *name, u8 *namelen) {
    if (len < 3 || len > 110 || !UOK(addr, len)) return 0;
    const u8 *a = (const u8 *)(usize)addr;
    if (*(const u16 *)a != 1) return 0;                          /* AF_UNIX */
    const char *path = (const char *)a + 2;
    u64 plen = len - 2;
    if (path[0] == 0) {                                           /* abstract: every byte counts */
        memcpy(name, path, plen);
        *namelen = (u8)plen;
        return 1;
    }
    char tmp[110];
    usize n = 0;
    while (n < plen && n < sizeof tmp - 1 && path[n]) { tmp[n] = path[n]; n++; }
    tmp[n] = 0;
    char full[256];
    path_abs(p, -100, tmp, full, sizeof full);
    usize fl = strlen(full);
    if (fl >= 108) return 0;
    memcpy(name, full, fl + 1);
    *namelen = (u8)(fl + 1);
    return 1;
}

static usock_t *lookup(const char *name, u8 namelen) {
    for (int i = 0; i < MAX_BOUND; i++)
        if (bound[i] && bound[i]->namelen == namelen && !memcmp(bound[i]->name, name, namelen)) return bound[i];
    return NULL;
}

i64 unix_bind(proc_t *p, ufile_t *f, u64 addr, u64 len) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (s->namelen) return -EINVAL;
    char name[108]; u8 nl;
    if (!parse_addr(p, addr, len, name, &nl)) return -EINVAL;
    if (lookup(name, nl)) return -EADDRINUSE;
    if (name[0] && vfs_lookup(name)) return -EADDRINUSE;          /* a stale socket file: the server unlinks it first */
    for (int i = 0; i < MAX_BOUND; i++)
        if (!bound[i]) {
            bound[i] = s;
            memcpy(s->name, name, nl);
            s->namelen = nl;
            if (name[0]) vfs_create(name, 0);                    /* the socket's file, so stat()/ls see it */
            return 0;
        }
    return -ENOMEM;
}

void unix_unlink_path(const char *path) {
    usize n = strlen(path) + 1;
    for (int i = 0; i < MAX_BOUND; i++)
        if (bound[i] && bound[i]->namelen == n && !memcmp(bound[i]->name, path, n)) { bound[i]->namelen = 0; bound[i] = NULL; }
}

i64 unix_listen(proc_t *p, ufile_t *f, int backlog) {
    (void)p;
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (s->type == ST_DGRAM) return -EOPNOTSUPP;
    if (s->connected) return -EINVAL;
    if (!s->listening) {
        s->maxback = CLAMP(backlog, 16, 512);
        s->backlog = kalloc((usize)s->maxback * sizeof(usock_t *));
        s->listening = 1;
    }
    return 0;
}

static i64 wait_until(proc_t *p, ufile_t *f, int flags, int (*ready)(usock_t *), usock_t *s) {
    for (;;) {
        if (ready(s)) return 0;
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
        if (proc_interrupted(p)) return -EINTR;
        thread_sleep_ms(1);
    }
}

static int has_backlog(usock_t *s) { return s->nback > 0; }

static void put_name(proc_t *p, u64 addr, u64 lenp, const char *name, u8 nl) {
    if (!addr || !lenp || !UOK(lenp, 4)) return;
    u32 cap = *(u32 *)(usize)lenp;
    u8 buf[110];
    *(u16 *)buf = 1;
    memcpy(buf + 2, name, nl);
    u32 full = 2u + nl;
    if (UOK(addr, MIN(cap, full))) memcpy((void *)(usize)addr, buf, MIN(cap, full));
    *(u32 *)(usize)lenp = full;
}

i64 unix_accept(proc_t *p, ufile_t *f, u64 addr, u64 lenp, int flags) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (!s->listening) return -EINVAL;
    i64 e = wait_until(p, f, 0, has_backlog, s);
    if (e) return e;
    u64 fl = irq_save();
    usock_t *c = s->backlog[0];
    memmove(s->backlog, s->backlog + 1, (usize)(--s->nback) * sizeof(usock_t *));
    irq_restore(fl);
    i64 fd = fd_install_obj(p, &c->h, flags);                     /* the backlog's reference moves to the descriptor */
    if (fd < 0) { kobj_put(&c->h); return fd; }
    put_name(p, addr, lenp, "", 0);
    return fd;
}

i64 unix_connect(proc_t *p, ufile_t *f, u64 addr, u64 len) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    char name[108]; u8 nl;
    if (!parse_addr(p, addr, len, name, &nl)) return -EINVAL;
    usock_t *t = lookup(name, nl);
    if (!t) return name[0] && !vfs_lookup(name) ? -ENOENT : -ECONNREFUSED;
    if (s->type != t->type) return -EPROTOTYPE;
    if (s->type == ST_DGRAM) { memcpy(s->dname, name, nl); s->dnamelen = nl; s->connected = 1; return 0; }   /* a default destination */
    if (s->connected) return -EISCONN;
    if (!t->listening) return -ECONNREFUSED;
    u64 fl = irq_save();
    if (t->nback >= t->maxback) { irq_restore(fl); return -EAGAIN; }
    usock_t *srv = us_new(p, s->type);                            /* the server's end, waiting for accept() */
    srv->pid = t->pid;
    memcpy(srv->name, t->name, t->namelen);
    srv->namelen = 0;
    srv->peer = s; s->peer = srv;
    srv->connected = s->connected = 1;
    t->backlog[t->nback++] = srv;
    irq_restore(fl);
    return 0;
}

/* ---- sending ------------------------------------------------------------------------------- */
/* gather the iovecs and descriptors of one send into a message for 'dst' */
static i64 do_send(proc_t *p, ufile_t *f, usock_t *s, const u64 *iov, u64 niov, const int *fds, int nfds, int flags,
                   const char *to, u8 tolen) {
    usock_t *dst;
    if (s->type == ST_DGRAM && (tolen || s->dnamelen)) {
        dst = tolen ? lookup(to, tolen) : lookup(s->dname, s->dnamelen);
        if (!dst) return -ECONNREFUSED;
    } else {
        if (s->shut_wr) goto pipe;
        dst = s->peer;
        if (!dst) {
            if (s->connected || s->peer_closed) goto pipe;
            return s->type == ST_DGRAM ? -ENOTCONN : -ENOTCONN;
        }
    }
    u64 total = 0;
    for (u64 i = 0; i < niov; i++) total += iov[i * 2 + 1];
    if (s->type != ST_STREAM && total > RCV_LIMIT) return -EMSGSIZE;
    /* room at the receiver */
    for (;;) {
        if (s->type != ST_DGRAM && !s->peer) goto pipe;
        if (dst->rxbytes == 0 || dst->rxbytes + total <= RCV_LIMIT || (s->type == ST_STREAM && dst->rxbytes < RCV_LIMIT)) break;
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
        if (proc_interrupted(p)) return -EINTR;
        thread_sleep_ms(1);
    }
    u64 n = total;
    if (s->type == ST_STREAM && dst->rxbytes && dst->rxbytes + n > RCV_LIMIT) n = RCV_LIMIT - dst->rxbytes;   /* a partial write */
    umsg_t *m = kalloc(sizeof *m + n);
    m->len = (u32)n;
    u64 at = 0;
    for (u64 i = 0; i < niov && at < n; i++) {
        u64 c = MIN(iov[i * 2 + 1], n - at);
        if (c && !UOK(iov[i * 2], c)) { kfree(m); return -EFAULT; }
        memcpy(m->data + at, (void *)(usize)iov[i * 2], c);
        at += c;
    }
    if (nfds) {
        m->fds = kalloc((usize)nfds * sizeof(ufile_t));
        for (int i = 0; i < nfds; i++) {
            if (fds[i] < 0 || fds[i] >= MAX_FDS || !p->fd[fds[i]].type) { for (int j = 0; j < i; j++) ufile_unref(&m->fds[j]); kfree(m->fds); kfree(m); return -EBADF; }
            m->fds[i] = p->fd[fds[i]];
            m->fds[i].cloexec = 0;
            ufile_ref(&m->fds[i]);                                 /* in flight: the message holds it */
        }
        m->nfds = nfds;
    }
    memcpy(m->from, s->name, s->namelen);
    m->fromlen = s->namelen;
    u64 fl = irq_save();
    if (dst->tail) dst->tail->next = m; else dst->head = m;
    dst->tail = m;
    dst->rxbytes += m->len;
    irq_restore(fl);
    return (i64)n;
pipe:
    if (!(flags & MSG_NOSIGNAL)) sig_post(p, thread_current(), 13, 0, p->pid, 0);   /* SIGPIPE */
    return -EPIPE;
}

i64 unix_sendto(proc_t *p, ufile_t *f, u64 buf, u64 len, int flags, u64 addr, u64 alen) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    char to[108]; u8 tl = 0;
    if (addr && alen && s->type == ST_DGRAM && !parse_addr(p, addr, alen, to, &tl)) return -EINVAL;
    u64 iov[2] = { buf, len };
    return do_send(p, f, s, iov, 1, NULL, 0, flags, to, tl);
}

i64 unix_sendmsg(proc_t *p, ufile_t *f, u64 msg, int flags) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (!UOK(msg, 56)) return -EFAULT;
    const u64 *mh = (const u64 *)(usize)msg;
    u64 name = mh[0], namelen = mh[1] & 0xffffffff, iov = mh[2], niov = mh[3], ctl = mh[4], ctllen = mh[5];
    if (niov > 1024 || (niov && !UOK(iov, niov * 16))) return -EFAULT;
    char to[108]; u8 tl = 0;
    if (name && namelen && s->type == ST_DGRAM && !parse_addr(p, name, namelen, to, &tl)) return -EINVAL;
    int fds[MAX_FDS_MSG], nfds = 0;
    if (ctl && ctllen) {
        if (!UOK(ctl, ctllen)) return -EFAULT;
        for (u64 o = 0; o + 16 <= ctllen;) {
            const u8 *c = (const u8 *)(usize)(ctl + o);
            u64 clen = *(const u64 *)c;
            int level = *(const i32 *)(c + 8), type = *(const i32 *)(c + 12);
            if (clen < 16 || o + clen > ctllen) break;
            if (level == 1 && type == 1)                           /* SOL_SOCKET, SCM_RIGHTS */
                for (u64 k = 16; k + 4 <= clen && nfds < MAX_FDS_MSG; k += 4) fds[nfds++] = *(const i32 *)(c + k);
            o += (clen + 7) & ~7ull;
        }
    }
    return do_send(p, f, s, (const u64 *)(usize)iov, niov, fds, nfds, flags, to, tl);
}

/* ---- receiving -------------------------------------------------------------------------------- */
static int readable(usock_t *s) { return s->head || s->peer_closed || s->shut_rd || (s->type != ST_DGRAM && s->connected && !s->peer); }

/* the common receive: data into the iovecs, descriptors into *got (the caller installs them) */
static i64 do_recv(proc_t *p, ufile_t *f, usock_t *s, const u64 *iov, u64 niov, int flags, int *mflags,
                   ufile_t **got, int *ngot, char *from, u8 *fromlen) {
    u64 want = 0;
    for (u64 i = 0; i < niov; i++) want += iov[i * 2 + 1];
    if (s->listening) return -EINVAL;
    if (s->type != ST_DGRAM && !s->connected && !s->peer_closed) return -ENOTCONN;
    u64 copied = 0;
    *ngot = 0;
    for (;;) {
        i64 e = wait_until(p, f, copied && !(flags & MSG_WAITALL) ? MSG_DONTWAIT : flags, readable, s);
        if (e) return copied ? (i64)copied : e;
        u64 fl = irq_save();
        umsg_t *m = s->head;
        if (!m) { irq_restore(fl); return (i64)copied; }          /* end of stream */
        if (copied && m->nfds) { irq_restore(fl); return (i64)copied; }
        u64 avail = m->len - m->off, n = MIN(avail, want - copied);
        /* copy into the iovecs at offset 'copied' */
        u64 pos = copied, done = 0;
        for (u64 i = 0; i < niov && done < n; i++) {
            u64 l = iov[i * 2 + 1];
            if (pos >= l) { pos -= l; continue; }
            u64 c = MIN(l - pos, n - done);
            if (!UOK(iov[i * 2] + pos, c)) { irq_restore(fl); return copied ? (i64)copied : -EFAULT; }
            memcpy((void *)(usize)(iov[i * 2] + pos), m->data + m->off + done, c);
            done += c;
            pos = 0;
        }
        copied += n;
        if (from) { memcpy(from, m->from, m->fromlen); *fromlen = m->fromlen; }
        int took_fds = 0;
        if (m->nfds && !(flags & MSG_PEEK)) {
            *got = m->fds; *ngot = m->nfds;
            m->fds = NULL; m->nfds = 0;
            took_fds = 1;
        }
        if (s->type != ST_STREAM) {
            if (n < avail) { *mflags |= MSG_TRUNC; if (flags & MSG_TRUNC) copied = avail; }
            if (!(flags & MSG_PEEK)) { s->head = m->next; if (!s->head) s->tail = NULL; s->rxbytes -= m->len; irq_restore(fl); msg_free(m); }
            else irq_restore(fl);
            return (i64)copied;
        }
        if (!(flags & MSG_PEEK)) {
            m->off += (u32)n;
            s->rxbytes -= (u32)n;
            if (m->off == m->len) { s->head = m->next; if (!s->head) s->tail = NULL; irq_restore(fl); msg_free(m); fl = irq_save(); }
        }
        irq_restore(fl);
        if (copied >= want || took_fds || (flags & MSG_PEEK)) return (i64)copied;
        if (!(flags & MSG_WAITALL) && !s->head) return (i64)copied;
    }
}

static void install_fds(proc_t *p, ufile_t *got, int n, int cloexec, int *fdout, int room) {
    for (int i = 0; i < n; i++) {
        int fd = i < room ? fd_alloc(p, 0) : -1;
        if (fd < 0) { ufile_unref(&got[i]); continue; }            /* no room: the descriptor is closed */
        p->fd[fd] = got[i];
        p->fd[fd].cloexec = cloexec;
        fdout[i] = fd;
    }
}

i64 unix_recvfrom(proc_t *p, ufile_t *f, u64 buf, u64 len, int flags, u64 addr, u64 alenp) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    u64 iov[2] = { buf, len };
    int mf = 0, ngot = 0;
    ufile_t *got = NULL;
    char from[108]; u8 fl = 0;
    i64 r = do_recv(p, f, s, iov, 1, flags, &mf, &got, &ngot, from, &fl);
    if (got) { for (int i = 0; i < ngot; i++) ufile_unref(&got[i]); kfree(got); }   /* plain recv: descriptors are dropped */
    if (r >= 0 && addr) put_name(p, addr, alenp, from, fl);
    return r;
}

i64 unix_recvmsg(proc_t *p, ufile_t *f, u64 msg, int flags) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (!UOK(msg, 56)) return -EFAULT;
    u64 *mh = (u64 *)(usize)msg;
    u64 name = mh[0], iov = mh[2], niov = mh[3], ctl = mh[4], ctllen = mh[5];
    if (niov > 1024 || (niov && !UOK(iov, niov * 16))) return -EFAULT;
    int mf = 0, ngot = 0;
    ufile_t *got = NULL;
    char from[108]; u8 fl = 0;
    i64 r = do_recv(p, f, s, (const u64 *)(usize)iov, niov, flags, &mf, &got, &ngot, from, &fl);
    u64 used = 0;
    if (got) {
        int room = ctl && ctllen >= 16 ? (int)((ctllen - 16) / 4) : 0;
        if (room < ngot) mf |= MSG_CTRUNC;
        int fds[MAX_FDS_MSG];
        install_fds(p, got, ngot, (flags & MSG_CMSG_CLOEXEC) != 0, fds, room);
        kfree(got);
        int n = MIN(ngot, room);
        if (n > 0 && UOK(ctl, 16 + 4 * (u64)n)) {
            u8 *c = (u8 *)(usize)ctl;
            *(u64 *)c = 16 + 4 * (u64)n;
            *(i32 *)(c + 8) = 1; *(i32 *)(c + 12) = 1;
            memcpy(c + 16, fds, 4 * (usize)n);
            used = (16 + 4 * (u64)n + 7) & ~7ull;
            if (used > ctllen) used = ctllen;
        }
    }
    mh[5] = used;                                                  /* msg_controllen */
    *(i32 *)(usize)(msg + 48) = mf;                                /* msg_flags */
    if (name && r >= 0) {
        u32 *nl = (u32 *)(usize)(msg + 8);
        u8 abuf[110];
        *(u16 *)abuf = 1;
        memcpy(abuf + 2, from, fl);
        u32 full = 2u + fl;
        if (UOK(name, MIN(*nl, full))) memcpy((void *)(usize)name, abuf, MIN(*nl, full));
        *nl = full;
    }
    return r;
}

/* ---- the rest -------------------------------------------------------------------------------- */
i64 unix_shutdown(proc_t *p, ufile_t *f, int how) {
    (void)p;
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (!s->connected && s->type != ST_DGRAM) return -ENOTCONN;
    if (how == 0 || how == 2) s->shut_rd = 1;
    if (how == 1 || how == 2) { s->shut_wr = 1; if (s->peer) s->peer->peer_closed = 1; }
    return 0;
}

i64 unix_getname(proc_t *p, ufile_t *f, u64 addr, u64 lenp, int peer) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (peer) {
        if (s->type == ST_DGRAM && s->dnamelen) put_name(p, addr, lenp, s->dname, s->dnamelen);
        else if (!s->peer) return -ENOTCONN;
        else put_name(p, addr, lenp, s->peer->name, s->peer->namelen);
    } else put_name(p, addr, lenp, s->name, s->namelen);
    return 0;
}

i64 unix_getsockopt(proc_t *p, ufile_t *f, int level, int opt, u64 val, u64 lenp) {
    usock_t *s = us_of(f);
    if (!s) return -ENOTSOCK;
    if (level != 1 || !UOK(lenp, 4)) return level != 1 ? -EOPNOTSUPP : -EFAULT;
    u32 cap = *(u32 *)(usize)lenp;
    i32 iv[3] = { 0 };
    u32 n = 4;
    switch (opt) {
    case 17:                                                       /* SO_PEERCRED: pid, uid, gid */
        if (!s->peer && !s->peer_closed) return -ENOTCONN;
        iv[0] = s->peer ? s->peer->pid : 0; n = 12; break;
    case 3: iv[0] = s->type; break;                                /* SO_TYPE */
    case 4: iv[0] = 0; break;                                      /* SO_ERROR */
    case 7: case 8: iv[0] = (i32)RCV_LIMIT; break;                 /* SO_SNDBUF, SO_RCVBUF */
    case 30: iv[0] = s->listening; break;                          /* SO_ACCEPTCONN */
    case 39: iv[0] = 1; break;                                     /* SO_DOMAIN: AF_UNIX */
    default: iv[0] = 0; break;
    }
    n = MIN(n, cap);
    if (!UOK(val, n)) return -EFAULT;
    memcpy((void *)(usize)val, iv, n);
    *(u32 *)(usize)lenp = n;
    return 0;
}

int unix_poll(proc_t *p, ufile_t *f, int events) {
    (void)p;
    usock_t *s = us_of(f);
    if (!s) return POLLERR;
    int rev = 0;
    if (s->listening) return s->nback ? (events & POLLIN) : 0;
    if (s->head) rev |= POLLIN;
    if (s->type != ST_DGRAM) {
        if (s->peer_closed || s->shut_rd) rev |= POLLIN | POLLRDHUP;
        if (s->peer_closed && (s->shut_wr || !s->peer)) rev |= POLLHUP;
        if (!s->connected && !s->peer_closed) rev |= POLLHUP;
        if (s->peer && !s->shut_wr && s->peer->rxbytes < RCV_LIMIT) rev |= POLLOUT;
    } else rev |= POLLOUT;
    return rev & (events | POLLHUP | POLLERR);
}

i64 unix_available(ufile_t *f) {
    usock_t *s = us_of(f);
    if (!s) return 0;
    if (s->type != ST_STREAM) return s->head ? s->head->len : 0;
    return s->rxbytes;
}
