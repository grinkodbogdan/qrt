/*
 * lsock.c - Linux sockets for QRT's Linux layer: AF_INET stream (TCP),
 * datagram (UDP) and ICMP (ping) sockets, poll/select, on top of src/net.
 *
 * The network stack runs on the shell thread; system calls take the net
 * lock around every call into it and sleep in short steps while they wait.
 */
#include "lsock.h"
#include "../../net/net.h"
#include "../../net/netstack.h"
#include "sched.h"

enum { EBADF = 9, EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EINVAL = 22, EMFILE = 24, ENOTSOCK = 88, EDESTADDRREQ = 89,
       EMSGSIZE = 90, EPROTONOSUPPORT = 93, EOPNOTSUPP = 95, EAFNOSUPPORT = 97, EADDRINUSE = 98, ENETUNREACH = 101,
       ECONNRESET = 104, EISCONN = 106, ENOTCONN = 107, ETIMEDOUT = 110, ECONNREFUSED = 111, EINPROGRESS = 115, EPIPE = 32,
       EHOSTUNREACH = 113, EINTR = 4 };
enum { K_STREAM = 1, K_DGRAM = 2, K_ICMP = 3 };

#define NSOCKS 32
#define QLEN   16

typedef struct { u32 ip; u16 port; u16 len; u8 data[1500]; } dgram_t;

typedef struct {
    int used, refs, kind, nonblock, raw;
    int tcp;                          /* tcp socket id */
    u16 lport; int bound;
    u32 pip; u16 pport; int connected;
    int err;                          /* pending SO_ERROR */
    dgram_t *q; int qh, qn;           /* datagram queue */
    u16 icmp_id;
} lsock_t;

static lsock_t socks[NSOCKS];

#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))

static lsock_t *get(proc_t *p, int fd) {
    if (fd < 0 || fd >= MAX_FDS || p->fd[fd].type != F_SOCK) return NULL;
    int s = p->fd[fd].sock;
    return s >= 0 && s < NSOCKS && socks[s].used ? &socks[s] : NULL;
}

/* ---- incoming datagrams (called with the net lock held, from the shell thread) ---- */
static void enqueue(lsock_t *s, u32 ip, u16 port, const u8 *d, usize len) {
    if (!s->q || s->qn >= QLEN || len > 1500) return;
    dgram_t *g = &s->q[(s->qh + s->qn) % QLEN];
    g->ip = ip; g->port = port; g->len = (u16)len;
    memcpy(g->data, d, len);
    s->qn++;
}

static void udp_rx(void *ctx, u32 ip, u16 port, const u8 *d, usize len) {
    lsock_t *s = ctx;
    if (s->connected && (ip != s->pip || port != s->pport)) return;
    enqueue(s, ip, port, d, len);
}

static void echo_rx(u32 ip, u16 id, u16 seq, const u8 *d, usize len) {
    /* hand the reply to every ICMP socket: raw ones get an IP header in front */
    for (int i = 0; i < NSOCKS; i++) {
        lsock_t *s = &socks[i];
        if (!s->used || s->kind != K_ICMP) continue;
        u8 pkt[1500];
        usize o = 0;
        if (s->raw) {
            netif_t *n = net_primary();
            memset(pkt, 0, 20);
            pkt[0] = 0x45; put16(pkt + 2, (u16)(20 + 8 + len)); pkt[8] = 64; pkt[9] = 1;
            put32(pkt + 12, ip); put32(pkt + 16, n ? n->ip : 0);
            put16(pkt + 10, net_checksum(pkt, 20, 0));
            o = 20;
        } else if (id != s->icmp_id) continue;
        if (o + 8 + len > sizeof pkt) continue;
        pkt[o] = 0; pkt[o + 1] = 0;
        put16(pkt + o + 4, id); put16(pkt + o + 6, seq);
        memcpy(pkt + o + 8, d, len);
        put16(pkt + o + 2, 0);
        put16(pkt + o + 2, net_checksum(pkt + o, 8 + len, 0));
        enqueue(s, ip, 0, pkt, o + 8 + len);
    }
}

/* ---- helpers ---------------------------------------------------------------------- */
static int read_sin(proc_t *p, u64 addr, u64 len, u32 *ip, u16 *port) {
    if (!addr || len < 8 || !UOK(addr, 8)) return -EFAULT;
    const u8 *a = (const u8 *)(usize)addr;
    u16 family = (u16)(a[0] | a[1] << 8);
    if (family != 2) return -EAFNOSUPPORT;
    *port = be16(a + 2);
    *ip = be32(a + 4);
    if (*ip == 0x7f000001u) return -ENETUNREACH;          /* no loopback interface */
    return 0;
}

static void write_sin(proc_t *p, u64 addr, u64 lenp, u32 ip, u16 port) {
    if (!addr || !lenp || !UOK(lenp, 4)) return;
    u32 cap = *(u32 *)(usize)lenp;
    u8 a[16] = { 2, 0 };
    put16(a + 2, port);
    put32(a + 4, ip);
    if (UOK(addr, MIN(cap, 16u))) memcpy((void *)(usize)addr, a, MIN(cap, 16u));
    *(u32 *)(usize)lenp = 16;
}

static void sock_release(lsock_t *s) {
    net_lock();
    if (s->kind == K_STREAM && s->tcp >= 0) tcp_close(s->tcp);
    if (s->kind == K_DGRAM && s->bound) udp_unbind(s->lport);
    net_unlock();
    if (s->q) kfree(s->q);
    memset(s, 0, sizeof *s);
}

void lsock_close(proc_t *p, int fd) {
    lsock_t *s = get(p, fd);
    p->fd[fd].type = F_NONE;
    if (s && --s->refs <= 0) sock_release(s);
}

void lsock_dup(proc_t *p, int fd) { lsock_t *s = get(p, fd); if (s) s->refs++; }
/* a reference held outside any descriptor table (a socket in flight over SCM_RIGHTS) */
void lsock_ref(int idx, int delta) {
    if (idx < 0 || idx >= NSOCKS || !socks[idx].used) return;
    socks[idx].refs += delta;
    if (socks[idx].refs <= 0) sock_release(&socks[idx]);
}

void lsock_exit(proc_t *p) {
    for (int fd = 0; fd < MAX_FDS; fd++) if (p->fd[fd].type == F_SOCK) lsock_close(p, fd);
}

/* wait (with the lock released) until cond() or timeout; -EINTR if the program is being stopped */
typedef int (*cond_fn)(lsock_t *s);
static int wait_for(proc_t *p, lsock_t *s, cond_fn c, u64 ms) {
    u64 end = ms ? k_now_ms() + ms : ~0ull;
    for (;;) {
        net_lock();
        int ok = c(s);
        net_unlock();
        if (ok) return 0;
        if (proc_interrupted(p)) return -EINTR;
        if (k_now_ms() >= end) return -ETIMEDOUT;
        thread_sleep_ms(5);
    }
}

static int c_connected(lsock_t *s) { int st = tcp_state(s->tcp); return st != TCP_SYN_SENT; }
static int c_readable(lsock_t *s) {
    if (s->kind == K_STREAM) return tcp_readable(s->tcp) > 0 || tcp_peer_closed(s->tcp);
    return s->qn > 0;
}
static int c_writable(lsock_t *s) {
    if (s->kind != K_STREAM) return 1;
    int st = tcp_state(s->tcp);
    return st == TCP_FAILED || st == TCP_CLOSED || (st >= TCP_ESTABLISHED && tcp_writable(s->tcp) > 0);
}

/* ---- system calls ------------------------------------------------------------------- */
i64 lsock_socket(proc_t *p, int domain, int type, int proto) {
    if (domain != 2) return -EAFNOSUPPORT;                 /* IPv4 only (glibc falls back) */
    int kind = type & 0xf, flags = type & ~0xf;
    int k2 = kind == 1 ? K_STREAM : (kind == 2 && proto == 1) ? K_ICMP : kind == 2 ? K_DGRAM : (kind == 3 && proto == 1) ? K_ICMP : 0;
    if (!k2) return -EPROTONOSUPPORT;
    int si = -1;
    for (int i = 0; i < NSOCKS; i++) if (!socks[i].used) { si = i; break; }
    if (si < 0) return -ENOMEM;
    int fd = -1;
    for (int i = 0; i < MAX_FDS; i++) if (!p->fd[i].type) { fd = i; break; }
    if (fd < 0) return -EMFILE;
    lsock_t *s = &socks[si];
    memset(s, 0, sizeof *s);
    s->used = 1; s->refs = 1; s->kind = k2; s->tcp = -1;
    s->nonblock = !!(flags & 04000);
    s->raw = kind == 3;
    if (k2 != K_STREAM) s->q = kalloc(sizeof(dgram_t) * QLEN);
    if (k2 == K_ICMP) {
        s->icmp_id = (u16)(0x5100 + si);
        net_lock();
        net_on_echo_reply(echo_rx);
        net_unlock();
    }
    p->fd[fd] = (ufile_t){ F_SOCK, NULL, 0, (flags & 04000) ? 04000 : 0, 0, si };
    return fd;
}

static int ensure_bound(lsock_t *s) {
    if (s->kind != K_DGRAM || s->bound) return 0;
    net_lock();
    u16 port = udp_ephemeral();
    int r = port && !udp_bind(port, udp_rx, s) ? 0 : -EADDRINUSE;
    net_unlock();
    if (!r) { s->lport = port; s->bound = 1; }
    return r;
}

i64 lsock_bind(proc_t *p, int fd, u64 addr, u64 len) {
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->kind != K_DGRAM) return 0;
    u32 ip; u16 port;
    if (!addr || len < 8 || !UOK(addr, 8)) return -EFAULT;
    port = be16((const u8 *)(usize)addr + 2);
    (void)ip;
    if (s->bound) return -EINVAL;
    if (!port) return ensure_bound(s);
    net_lock();
    int r = udp_bind(port, udp_rx, s) ? -EADDRINUSE : 0;
    net_unlock();
    if (!r) { s->lport = port; s->bound = 1; }
    return r;
}

i64 lsock_connect(proc_t *p, int fd, u64 addr, u64 len) {
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    u32 ip; u16 port;
    int r = read_sin(p, addr, len, &ip, &port);
    if (r) return r;
    if (s->kind != K_STREAM) {
        s->pip = ip; s->pport = port; s->connected = 1;
        return ensure_bound(s);
    }
    if (s->tcp >= 0) return tcp_state(s->tcp) == TCP_SYN_SENT ? -EINPROGRESS : -EISCONN;
    net_lock();
    if (!net_primary()) { net_unlock(); return -ENETUNREACH; }
    s->tcp = tcp_connect(ip, port);
    net_unlock();
    if (s->tcp < 0) return -ENOMEM;
    s->pip = ip; s->pport = port;
    if (s->nonblock) return -EINPROGRESS;
    r = wait_for(p, s, c_connected, 30000);
    if (r) return r;
    net_lock();
    int st = tcp_state(s->tcp);
    net_unlock();
    if (st != TCP_ESTABLISHED) { s->err = ECONNREFUSED; return -ECONNREFUSED; }
    s->connected = 1;
    return 0;
}

static i64 stream_send(proc_t *p, lsock_t *s, const u8 *buf, usize len) {
    usize done = 0;
    while (done < len) {
        net_lock();
        int st = tcp_state(s->tcp);
        int n = (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) ? tcp_send(s->tcp, buf + done, len - done) : -1;
        net_unlock();
        if (n < 0) return done ? (i64)done : -EPIPE;
        done += (usize)n;
        if (done < len) {
            if (s->nonblock) return done ? (i64)done : -EAGAIN;
            int r = wait_for(p, s, c_writable, 30000);
            if (r) return done ? (i64)done : r;
        }
    }
    return (i64)done;
}

i64 lsock_sendto(proc_t *p, int fd, u64 buf, u64 len, int flags, u64 addr, u64 alen) {
    (void)flags;
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    if (len && !UOK(buf, len)) return -EFAULT;
    const u8 *b = (const u8 *)(usize)buf;
    if (s->kind == K_STREAM) {
        if (s->tcp < 0) return -ENOTCONN;
        return stream_send(p, s, b, len);
    }
    u32 ip = s->pip; u16 port = s->pport;
    if (addr) { int r = read_sin(p, addr, alen, &ip, &port); if (r) return r; }
    else if (!s->connected) return -EDESTADDRREQ;
    if (len > 1472) return -EMSGSIZE;
    net_lock();
    int r;
    if (!net_primary()) r = -ENETUNREACH;
    else if (s->kind == K_ICMP) {
        u8 m[1480];
        memcpy(m, b, len);
        if (len >= 8) {
            if (!s->raw) put16(m + 4, s->icmp_id);
            put16(m + 2, 0);
            put16(m + 2, net_checksum(m, len, 0));
        }
        r = ip_output(ip, 1, m, len) < 0 ? -EHOSTUNREACH : 0;
    } else {
        net_unlock();
        if ((r = ensure_bound(s))) return r;
        net_lock();
        r = udp_send(ip, s->lport, port, b, len) < 0 ? -EHOSTUNREACH : 0;
    }
    net_unlock();
    return r ? r : (i64)len;
}

i64 lsock_recvfrom(proc_t *p, int fd, u64 buf, u64 len, int flags, u64 addr, u64 alenp) {
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    if (len && !UOK(buf, len)) return -EFAULT;
    int dontwait = s->nonblock || (flags & 0x40);
    if (s->kind == K_STREAM && s->tcp < 0) return -ENOTCONN;
    net_lock();
    int ready = c_readable(s);
    net_unlock();
    if (!ready) {
        if (dontwait) return -EAGAIN;
        int r = wait_for(p, s, c_readable, 0);
        if (r) return r;
    }
    net_lock();
    i64 n;
    if (s->kind == K_STREAM) {
        n = tcp_recv(s->tcp, (void *)(usize)buf, len);
        if (n <= 0) n = tcp_state(s->tcp) == TCP_FAILED ? -ECONNRESET : 0;
        net_unlock();
        if (addr) write_sin(p, addr, alenp, s->pip, s->pport);
        return n;
    }
    dgram_t *g = &s->q[s->qh];
    n = (i64)MIN((u64)g->len, len);
    memcpy((void *)(usize)buf, g->data, (usize)n);
    u32 ip = g->ip; u16 port = g->port;
    if (!(flags & 2)) { s->qh = (s->qh + 1) % QLEN; s->qn--; }     /* MSG_PEEK keeps it */
    net_unlock();
    if (addr) write_sin(p, addr, alenp, ip, port);
    return n;
}

/* msghdr: name, namelen, iov, iovlen, control, controllen, flags */
typedef struct { u64 name; u32 namelen, pad; u64 iov, iovlen, control, controllen; i32 flags; } msghdr_t;
typedef struct { u64 base, len; } iovec_t;

i64 lsock_sendmsg(proc_t *p, int fd, u64 msg, int flags) {
    if (!UOK(msg, sizeof(msghdr_t))) return -EFAULT;
    msghdr_t *m = (msghdr_t *)(usize)msg;
    if (m->iovlen > 16 || !UOK(m->iov, m->iovlen * sizeof(iovec_t))) return -EINVAL;
    iovec_t *v = (iovec_t *)(usize)m->iov;
    static u8 buf[65536];
    usize total = 0;
    for (u64 i = 0; i < m->iovlen; i++) {
        if (total + v[i].len > sizeof buf || (v[i].len && !UOK(v[i].base, v[i].len))) return -EMSGSIZE;
        memcpy(buf + total, (void *)(usize)v[i].base, v[i].len);
        total += v[i].len;
    }
    /* the copy lives in kernel memory: send it straight to the stack */
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->kind == K_STREAM) return s->tcp < 0 ? -ENOTCONN : stream_send(p, s, buf, total);
    u32 ip = s->pip; u16 port = s->pport;
    if (m->name) { int r = read_sin(p, m->name, m->namelen, &ip, &port); if (r) return r; }
    else if (!s->connected) return -EDESTADDRREQ;
    (void)flags;
    if (total > 1472) return -EMSGSIZE;
    int r = ensure_bound(s);
    if (r) return r;
    net_lock();
    if (s->kind == K_ICMP) {
        if (total >= 8) { if (!s->raw) put16(buf + 4, s->icmp_id); put16(buf + 2, 0); put16(buf + 2, net_checksum(buf, total, 0)); }
        r = ip_output(ip, 1, buf, total) < 0 ? -EHOSTUNREACH : 0;
    } else r = net_primary() && udp_send(ip, s->lport, port, buf, total) >= 0 ? 0 : -EHOSTUNREACH;
    net_unlock();
    return r ? r : (i64)total;
}

i64 lsock_sendmmsg(proc_t *p, int fd, u64 vec, u32 n, int flags) {
    /* struct mmsghdr { struct msghdr; unsigned msg_len; } = 64 bytes */
    for (u32 i = 0; i < n; i++) {
        u64 m = vec + (u64)i * 64;
        if (!UOK(m, 64)) return i ? (i64)i : -EFAULT;
        i64 r = lsock_sendmsg(p, fd, m, flags);
        if (r < 0) return i ? (i64)i : r;
        *(u32 *)(usize)(m + 56) = (u32)r;
    }
    return n;
}

i64 lsock_recvmsg(proc_t *p, int fd, u64 msg, int flags) {
    if (!UOK(msg, sizeof(msghdr_t))) return -EFAULT;
    msghdr_t *m = (msghdr_t *)(usize)msg;
    if (m->iovlen < 1 || m->iovlen > 16 || !UOK(m->iov, m->iovlen * sizeof(iovec_t))) return -EINVAL;
    iovec_t *v = (iovec_t *)(usize)m->iov;
    u32 nl = m->namelen;
    static u8 buf[65536];
    usize cap = 0;
    for (u64 i = 0; i < m->iovlen; i++) cap += v[i].len;
    cap = MIN(cap, sizeof buf);
    /* receive into our buffer (the address pointer is checked by recvfrom) */
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    u64 tmp_len = 0;
    i64 n;
    {
        /* reuse recvfrom by pointing it at a kernel buffer: temporarily trust it */
        int dontwait = s->nonblock || (flags & 0x40);
        net_lock();
        int ready = c_readable(s);
        net_unlock();
        if (!ready) {
            if (dontwait) return -EAGAIN;
            int r = wait_for(p, s, c_readable, 0);
            if (r) return r;
        }
        net_lock();
        u32 ip = s->pip; u16 port = s->pport;
        if (s->kind == K_STREAM) { n = tcp_recv(s->tcp, buf, cap); if (n < 0) n = 0; }
        else {
            dgram_t *g = &s->q[s->qh];
            n = (i64)MIN((usize)g->len, cap);
            memcpy(buf, g->data, (usize)n);
            ip = g->ip; port = g->port;
            if (g->len > cap) m->flags |= 0x20;            /* MSG_TRUNC */
            if (!(flags & 2)) { s->qh = (s->qh + 1) % QLEN; s->qn--; }
        }
        net_unlock();
        if (m->name && nl >= 16 && UOK(m->name, 16)) {
            u8 a[16] = { 2, 0 };
            put16(a + 2, port); put32(a + 4, ip);
            memcpy((void *)(usize)m->name, a, 16);
            m->namelen = 16;
        }
        tmp_len = (u64)n;
    }
    usize off = 0;
    for (u64 i = 0; i < m->iovlen && off < tmp_len; i++) {
        usize c = (usize)MIN(v[i].len, tmp_len - off);
        if (c && !UOK(v[i].base, c)) return -EFAULT;
        memcpy((void *)(usize)v[i].base, buf + off, c);
        off += c;
    }
    m->controllen = 0;
    return n;
}

i64 lsock_shutdown(proc_t *p, int fd, int how) {
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->kind == K_STREAM && s->tcp >= 0 && how != 0) { net_lock(); tcp_close(s->tcp); net_unlock(); s->tcp = -1; }
    return 0;
}

i64 lsock_getname(proc_t *p, int fd, u64 addr, u64 lenp, int peer) {
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    if (peer) { if (!s->connected && s->tcp < 0) return -ENOTCONN; write_sin(p, addr, lenp, s->pip, s->pport); }
    else { netif_t *n = net_primary(); write_sin(p, addr, lenp, n ? n->ip : 0, s->lport); }
    return 0;
}

i64 lsock_getsockopt(proc_t *p, int fd, int level, int opt, u64 val, u64 lenp) {
    lsock_t *s = get(p, fd);
    if (!s) return -ENOTSOCK;
    int v = 0;
    if (level == 1 && opt == 4) {                           /* SO_ERROR */
        if (s->kind == K_STREAM && s->tcp >= 0) {
            net_lock();
            int st = tcp_state(s->tcp);
            net_unlock();
            if (st == TCP_FAILED) v = ECONNREFUSED;
            else if (st == TCP_ESTABLISHED) s->connected = 1;
        }
        if (s->err) { v = s->err; s->err = 0; }
    } else if (level == 1 && opt == 3) v = s->kind == K_STREAM ? 1 : 2;   /* SO_TYPE */
    if (val && lenp && UOK(lenp, 4) && UOK(val, 4)) { *(int *)(usize)val = v; *(u32 *)(usize)lenp = 4; }
    return 0;
}

int lsock_readable(proc_t *p, int fd, int *hup) {
    lsock_t *s = get(p, fd);
    if (!s) return 0;
    net_lock();
    int r = c_readable(s);
    *hup = s->kind == K_STREAM && s->tcp >= 0 && tcp_peer_closed(s->tcp);
    net_unlock();
    return r;
}

int lsock_writable(proc_t *p, int fd) {
    lsock_t *s = get(p, fd);
    if (!s) return 0;
    if (s->kind == K_STREAM && s->tcp < 0) return 0;
    net_lock();
    int r = c_writable(s);
    net_unlock();
    return r;
}

i64 lsock_available(proc_t *p, int fd) {
    lsock_t *s = get(p, fd);
    if (!s) return 0;
    net_lock();
    i64 n = s->kind == K_STREAM ? (s->tcp >= 0 ? tcp_readable(s->tcp) : 0) : (s->qn ? s->q[s->qh].len : 0);
    net_unlock();
    return n;
}

void lsock_set_nonblock(proc_t *p, int fd, int on) { lsock_t *s = get(p, fd); if (s) s->nonblock = on; }
