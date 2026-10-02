/*
 * lfile.c - kernel objects behind Linux descriptors: shared memory (memfd_create,
 * /dev/shm, MAP_SHARED), eventfd, timerfd, signalfd and epoll - the pieces glib,
 * libevent, Wayland and Firefox's IPC build their event loops and buffers from.
 *
 * Every object is reference counted (kobj_t): descriptors in any process, copies
 * in flight over Unix sockets, and mappings each hold one.  Waiting is done the
 * way the rest of the Linux layer waits: sleep a little, look again, give up with
 * -EINTR when proc_interrupted() says a signal or a kill is due.
 */
#include "lfile.h"
#include "mm.h"

enum { EINTR = 4, EBADF = 9, EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EEXIST = 17, EINVAL = 22, ENOENT = 2, EPERM = 1, ELOOP = 40 };
#define O_NONBLOCK 04000
#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))
enum { POLLIN = 1, POLLOUT = 4, POLLERR = 8, POLLHUP = 0x10 };

void kobj_get(kobj_t *o) { u64 fl = irq_save(); o->refs++; irq_restore(fl); }

static void shm_free(kobj_t *o);
static void epoll_free(kobj_t *o);

void kobj_put(kobj_t *o) {
    if (!o) return;
    u64 fl = irq_save();
    int left = --o->refs;
    irq_restore(fl);
    if (left > 0) return;
    switch (o->kind) {
    case KO_SHM: shm_free(o); break;
    case KO_EPOLL: epoll_free(o); break;
    case KO_UNIX: unix_release(o); break;
    default: kfree(o); break;
    }
}

/* ---- shared memory -----------------------------------------------------------------------
 * The pages are frames of the kernel's own (>= 1 GiB) pool, so the kernel reads and
 * writes them directly (read/write on a memfd now, a compositor reading client
 * buffers later); processes map the same frames, marked shared. */
typedef struct {
    kobj_t h;
    u64 size;
    u64 *frames;
    u64 cap;                                 /* entries in frames */
    char name[64];
    int named;                               /* listed under /dev/shm (holds a reference) */
    void *ext;                               /* shm_wrap: the frames are this kernel allocation's */
} shm_t;

#define MAX_NAMED 64
static shm_t *named[MAX_NAMED];

kobj_t *shm_new(const char *name) {
    shm_t *s = kalloc(sizeof *s);
    s->h.kind = KO_SHM;
    s->h.refs = 1;
    strlcpy(s->name, name ? name : "", sizeof s->name);
    return &s->h;
}

static void shm_free(kobj_t *o) {
    shm_t *s = (shm_t *)o;
    if (s->ext) kfree(s->ext);
    else for (u64 i = 0; i < s->cap; i++) if (s->frames[i]) pmm_free(s->frames[i]);
    if (s->frames) kfree(s->frames);
    kfree(s);
}

u64 shm_size(kobj_t *o) { return ((shm_t *)o)->size; }

static int shm_reserve(shm_t *s, u64 pages) {
    if (pages <= s->cap) return 1;
    if (pages > (8ull << 30) / PAGE) return 0;               /* 8 GiB is plenty */
    u64 ncap = MAX(pages, s->cap * 2);
    u64 *nf = kalloc(ncap * sizeof(u64));
    if (s->frames) { memcpy(nf, s->frames, s->cap * sizeof(u64)); kfree(s->frames); }
    s->frames = nf;
    s->cap = ncap;
    return 1;
}

/* bytes of contiguous kernel memory, page aligned, shareable with programs (window
 * buffers: the shell reads the pixels where the program draws them); freed with the object */
kobj_t *shm_wrap(u64 bytes, void **mem) {
    u64 pages = (bytes + PAGE - 1) / PAGE;
    u8 *raw = kalloc((usize)(pages + 1) * PAGE);
    u8 *al = (u8 *)(((usize)raw + PAGE - 1) & ~(usize)(PAGE - 1));
    shm_t *s = (shm_t *)shm_new("window");
    s->ext = raw;
    shm_reserve(s, pages);
    for (u64 i = 0; i < pages; i++) s->frames[i] = (u64)(usize)al + i * PAGE;
    s->size = pages * PAGE;
    *mem = al;
    return &s->h;
}

u64 shm_frame(kobj_t *o, u64 page) {
    shm_t *s = (shm_t *)o;
    /* a mapping may run past the end of the object (Linux: SIGBUS); give it a page anyway */
    if (!shm_reserve(s, page + 1)) return 0;
    if (!s->frames[page]) s->frames[page] = pmm_alloc(1);
    return s->frames[page];
}

i64 shm_truncate(kobj_t *o, u64 size) {
    shm_t *s = (shm_t *)o;
    if (s->ext) return size <= s->size ? 0 : -EPERM;         /* a window buffer has its size */
    u64 pages = (size + PAGE - 1) / PAGE;
    if (!shm_reserve(s, pages)) return -ENOMEM;
    if (size < s->size) {
        /* pages past the new end go (mappings of them keep their frame until unmapped - a leak Linux avoids with SIGBUS) */
        if (size % PAGE && s->frames[size / PAGE]) memset((u8 *)(usize)s->frames[size / PAGE] + size % PAGE, 0, PAGE - size % PAGE);
    }
    s->size = size;
    return 0;
}

i64 shm_rw(kobj_t *o, u64 off, void *buf, u64 len, int write) {
    shm_t *s = (shm_t *)o;
    if (!write) { if (off >= s->size) return 0; len = MIN(len, s->size - off); }
    else if (off + len > s->size) { i64 e = shm_truncate(o, off + len); if (e) return e; }
    u8 *b = buf;
    u64 done = 0;
    while (done < len) {
        u64 pos = off + done, page = pos / PAGE, in = pos % PAGE, n = MIN(len - done, PAGE - in);
        u64 f = shm_frame(o, page);
        if (!f) return done ? (i64)done : -ENOMEM;
        if (write) memcpy((u8 *)(usize)f + in, b + done, n);
        else memcpy(b + done, (u8 *)(usize)f + in, n);
        done += n;
    }
    return (i64)done;
}

static shm_t *find_named(const char *name) {
    for (int i = 0; i < MAX_NAMED; i++) if (named[i] && !strcmp(named[i]->name, name)) return named[i];
    return NULL;
}

int shm_named_exists(const char *name) { return find_named(name) != NULL; }

kobj_t *shm_named(const char *name, int create, int excl, i64 *err) {
    shm_t *s = find_named(name);
    if (s) {
        if (create && excl) { *err = -EEXIST; return NULL; }
        kobj_get(&s->h);
        return &s->h;
    }
    if (!create) { *err = -ENOENT; return NULL; }
    for (int i = 0; i < MAX_NAMED; i++)
        if (!named[i]) {
            kobj_t *o = shm_new(name);
            s = (shm_t *)o;
            s->named = 1;
            named[i] = s;
            kobj_get(o);                                         /* the name's reference */
            return o;
        }
    *err = -ENOMEM;
    return NULL;
}

i64 shm_unlink(const char *name) {
    for (int i = 0; i < MAX_NAMED; i++)
        if (named[i] && !strcmp(named[i]->name, name)) {
            shm_t *s = named[i];
            named[i] = NULL;
            s->named = 0;
            kobj_put(&s->h);
            return 0;
        }
    return -ENOENT;
}

int shm_list(int i, char *name, usize cap, u64 *size) {
    for (int k = 0; k < MAX_NAMED; k++) {
        if (!named[k]) continue;
        if (i-- == 0) { strlcpy(name, named[k]->name, cap); *size = named[k]->size; return 1; }
    }
    return 0;
}

/* ---- eventfd ---------------------------------------------------------------------------------- */
typedef struct { kobj_t h; u64 count; int semaphore; } eventfd_t;

kobj_t *eventfd_new(u64 init, int semaphore) {
    eventfd_t *e = kalloc(sizeof *e);
    e->h.kind = KO_EVENTFD; e->h.refs = 1;
    e->count = init; e->semaphore = semaphore;
    return &e->h;
}

static i64 eventfd_read(proc_t *p, ufile_t *f, eventfd_t *e, u64 buf, u64 len) {
    if (len < 8) return -EINVAL;
    if (!UOK(buf, 8)) return -EFAULT;
    for (;;) {
        u64 fl = irq_save();
        if (e->count) {
            u64 v = e->semaphore ? 1 : e->count;
            e->count -= v;
            irq_restore(fl);
            *(u64 *)(usize)buf = v;
            return 8;
        }
        irq_restore(fl);
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (proc_interrupted(p)) return -EINTR;
        thread_sleep_ms(1);
    }
}

static i64 eventfd_write(proc_t *p, ufile_t *f, eventfd_t *e, u64 buf, u64 len) {
    if (len < 8) return -EINVAL;
    if (!UOK(buf, 8)) return -EFAULT;
    u64 v = *(u64 *)(usize)buf;
    if (v == ~0ull) return -EINVAL;
    for (;;) {
        u64 fl = irq_save();
        if (e->count + v >= e->count && e->count + v < ~0ull) { e->count += v; irq_restore(fl); return 8; }
        irq_restore(fl);
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (proc_interrupted(p)) return -EINTR;
        thread_sleep_ms(1);
    }
}

/* ---- timerfd --------------------------------------------------------------------------------- */
typedef struct { kobj_t h; int clock; u64 next_us, every_us, ticks; } timerfd_t;   /* next_us: k_now_us() time, 0 = off */

kobj_t *timerfd_new(int clock) {
    timerfd_t *t = kalloc(sizeof *t);
    t->h.kind = KO_TIMERFD; t->h.refs = 1;
    t->clock = clock;
    return &t->h;
}

static void timer_update(timerfd_t *t) {
    u64 now = k_now_us();
    if (!t->next_us || now < t->next_us) return;
    if (t->every_us) {
        u64 n = 1 + (now - t->next_us) / t->every_us;
        t->ticks += n;
        t->next_us += n * t->every_us;
    } else { t->ticks++; t->next_us = 0; }
}

static u64 realtime_us(void) { return k.epoch_at_boot * 1000000ull + k_now_us(); }
static u64 ts_us(const u64 *ts) { return ts[0] * 1000000ull + ts[1] / 1000; }

i64 timerfd_gettime(proc_t *p, kobj_t *o, u64 cur) {
    timerfd_t *t = (timerfd_t *)o;
    if (!UOK(cur, 32)) return -EFAULT;
    timer_update(t);
    u64 *c = (u64 *)(usize)cur, now = k_now_us(), left = t->next_us > now ? t->next_us - now : 0;
    c[0] = t->every_us / 1000000; c[1] = t->every_us % 1000000 * 1000;
    c[2] = left / 1000000; c[3] = left % 1000000 * 1000;
    if (t->next_us && !left) c[3] = 1;                           /* armed, due now */
    return 0;
}

i64 timerfd_settime(proc_t *p, kobj_t *o, int flags, u64 nv, u64 ov) {
    timerfd_t *t = (timerfd_t *)o;
    if (ov && timerfd_gettime(p, o, ov)) return -EFAULT;
    if (!UOK(nv, 32)) return -EFAULT;
    const u64 *n = (const u64 *)(usize)nv;
    if (n[1] >= 1000000000ull || n[3] >= 1000000000ull) return -EINVAL;
    u64 val = ts_us(n + 2);
    t->every_us = ts_us(n);
    t->ticks = 0;
    if (!val && !n[3]) { t->next_us = 0; return 0; }
    if (flags & 1) {                                              /* TFD_TIMER_ABSTIME */
        u64 now_clock = t->clock == 0 ? realtime_us() : k_now_us();
        u64 target = ts_us(n + 2);
        t->next_us = k_now_us() + (target > now_clock ? target - now_clock : 0);
        if (!t->next_us) t->next_us = 1;
    } else t->next_us = k_now_us() + MAX(val, (u64)1);
    return 0;
}

static i64 timerfd_read(proc_t *p, ufile_t *f, timerfd_t *t, u64 buf, u64 len) {
    if (len < 8) return -EINVAL;
    if (!UOK(buf, 8)) return -EFAULT;
    for (;;) {
        timer_update(t);
        if (t->ticks) { *(u64 *)(usize)buf = t->ticks; t->ticks = 0; return 8; }
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (proc_interrupted(p)) return -EINTR;
        u64 now = k_now_us();
        thread_sleep_ms(t->next_us > now ? MIN((t->next_us - now) / 1000 + 1, (u64)10) : 10);
    }
}

/* ---- signalfd -------------------------------------------------------------------------------- */
typedef struct { kobj_t h; u64 mask; } signalfd_t;

kobj_t *signalfd_new(u64 mask) {
    signalfd_t *s = kalloc(sizeof *s);
    s->h.kind = KO_SIGNALFD; s->h.refs = 1;
    s->mask = mask & ~((1ull << 8) | (1ull << 18));               /* never KILL or STOP */
    return &s->h;
}
void signalfd_set(kobj_t *o, u64 mask) { ((signalfd_t *)o)->mask = mask & ~((1ull << 8) | (1ull << 18)); }

static int signalfd_ready(proc_t *p, signalfd_t *s) {
    thread_t *t = thread_current();
    return ((t->sig_pending | p->sig_pending) & s->mask) != 0;
}

static i64 signalfd_read(proc_t *p, ufile_t *f, signalfd_t *s, u64 buf, u64 len) {
    if (len < 128) return -EINVAL;
    if (!UOK(buf, len)) return -EFAULT;
    u64 done = 0;
    for (;;) {
        ksiginfo_t in;
        int sig;
        while (done + 128 <= len && (sig = sig_take(p, s->mask, &in))) {
            u8 *o = (u8 *)(usize)(buf + done);
            memset(o, 0, 128);
            *(u32 *)o = (u32)sig;                                  /* ssi_signo */
            *(i32 *)(o + 8) = in.code;                             /* ssi_code */
            *(u32 *)(o + 12) = (u32)in.pid;                        /* ssi_pid */
            *(i32 *)(o + 40) = in.status;                          /* ssi_status */
            *(u64 *)(o + 72) = in.addr;                            /* ssi_addr */
            done += 128;
        }
        if (done) return (i64)done;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (p->killed || (sig_deliverable(p) & ~s->mask)) return -EINTR;
        thread_sleep_ms(5);
    }
}

/* ---- epoll ----------------------------------------------------------------------------------
 * The interest list is keyed by descriptor number; each entry remembers which open file it
 * was registered for, and is dropped when that descriptor is closed or reused (Linux drops
 * it when the last reference to the file goes - the same for programs that close what they
 * registered).  Edge-triggered entries are reported like level-triggered ones: an EPOLLET
 * user reads until EAGAIN anyway, so extra reports cost a call, never an event. */
typedef struct { int fd; u32 events; u64 data; const void *key; int disabled; } epitem_t;
typedef struct { kobj_t h; epitem_t *items; int n, cap; } epoll_t;

#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)

kobj_t *epoll_new(void) {
    epoll_t *e = kalloc(sizeof *e);
    e->h.kind = KO_EPOLL; e->h.refs = 1;
    return &e->h;
}

static void epoll_free(kobj_t *o) {
    epoll_t *e = (epoll_t *)o;
    if (e->items) kfree(e->items);
    kfree(e);
}

static const void *file_key(ufile_t *f) {
    switch (f->type) {
    case F_OBJ: return f->obj;
    case F_PIPE: return f->pipe;
    case F_FILE: case F_DIR: return f->vn;
    case F_SOCK: return (const void *)(usize)(0x1000 + f->sock);
    default: return (const void *)(usize)(0x100 + f->type);
    }
}

i64 epoll_ctl(proc_t *p, kobj_t *o, int op, int fd, u64 ev) {
    epoll_t *e = (epoll_t *)o;
    if (fd < 0 || fd >= MAX_FDS || !p->fd[fd].type) return -EBADF;
    if (p->fd[fd].type == F_OBJ && p->fd[fd].obj == o) return -EINVAL;
    if (p->fd[fd].type == F_FILE || p->fd[fd].type == F_DIR) return -EPERM;   /* Linux: regular files cannot be polled */
    u32 events = 0;
    u64 data = 0;
    if (op != 2) {                                                /* ADD, MOD read the event */
        if (!UOK(ev, 12)) return -EFAULT;
        events = *(u32 *)(usize)ev;
        data = *(u64 *)(usize)(ev + 4);
    }
    const void *key = file_key(&p->fd[fd]);
    int at = -1;
    for (int i = 0; i < e->n; i++) if (e->items[i].fd == fd && e->items[i].key == key) at = i;
    switch (op) {
    case 1:                                                       /* EPOLL_CTL_ADD */
        if (at >= 0) return -EEXIST;
        if (e->n == e->cap) {
            int nc = e->cap ? e->cap * 2 : 16;
            epitem_t *ni = kalloc((usize)nc * sizeof *ni);
            if (e->items) { memcpy(ni, e->items, (usize)e->n * sizeof *ni); kfree(e->items); }
            e->items = ni; e->cap = nc;
        }
        e->items[e->n++] = (epitem_t){ fd, events, data, key, 0 };
        return 0;
    case 2:                                                       /* DEL */
        if (at < 0) return -ENOENT;
        e->items[at] = e->items[--e->n];
        return 0;
    case 3:                                                       /* MOD */
        if (at < 0) return -ENOENT;
        e->items[at].events = events; e->items[at].data = data; e->items[at].disabled = 0;
        return 0;
    }
    return -EINVAL;
}

/* ready events into the user's array (NULL: just count) */
static int epoll_scan(proc_t *p, epoll_t *e, u8 *out, int max) {
    int n = 0;
    for (int i = 0; i < e->n && n < max; i++) {
        epitem_t *it = &e->items[i];
        ufile_t *f = it->fd < MAX_FDS ? &p->fd[it->fd] : NULL;
        if (!f || !f->type || file_key(f) != it->key) { e->items[i--] = e->items[--e->n]; continue; }   /* closed */
        if (it->disabled) continue;
        int rev = fd_poll(p, it->fd, (int)(it->events & 0xffff) | POLLERR | POLLHUP);
        rev &= (int)(it->events | POLLERR | POLLHUP);
        if (!rev) continue;
        if (out) {
            *(u32 *)(out + n * 12) = (u32)rev;
            *(u64 *)(out + n * 12 + 4) = it->data;
            if (it->events & EPOLLONESHOT) it->disabled = 1;
        }
        n++;
    }
    return n;
}

i64 epoll_wait(proc_t *p, kobj_t *o, u64 events, int max, i64 timeout_ms) {
    epoll_t *e = (epoll_t *)o;
    if (max <= 0 || max > 1 << 20) return -EINVAL;
    if (!UOK(events, (u64)max * 12)) return -EFAULT;
    u64 end = timeout_ms < 0 ? ~0ull : k_now_ms() + (u64)timeout_ms;
    for (;;) {
        int n = epoll_scan(p, e, (u8 *)(usize)events, max);
        if (n || k_now_ms() >= end) return n;
        if (proc_interrupted(p)) return -EINTR;
        u64 left = end - k_now_ms();
        thread_sleep_ms(MIN(left, (u64)2));
    }
}

/* ---- any object as a descriptor ------------------------------------------------------------------ */
i64 kobj_read(proc_t *p, ufile_t *f, u64 buf, u64 len) {
    kobj_t *o = f->obj;
    switch (o->kind) {
    case KO_EVENTFD: return eventfd_read(p, f, (eventfd_t *)o, buf, len);
    case KO_TIMERFD: return timerfd_read(p, f, (timerfd_t *)o, buf, len);
    case KO_SIGNALFD: return signalfd_read(p, f, (signalfd_t *)o, buf, len);
    case KO_UNIX: return unix_recvfrom(p, f, buf, len, 0, 0, 0);
    case KO_SHM: {
        if (!UOK(buf, len)) return -EFAULT;
        i64 r = shm_rw(o, f->off, (void *)(usize)buf, len, 0);
        if (r > 0) f->off += (u64)r;
        return r;
    }
    }
    return -EINVAL;
}

i64 kobj_write(proc_t *p, ufile_t *f, u64 buf, u64 len) {
    kobj_t *o = f->obj;
    switch (o->kind) {
    case KO_EVENTFD: return eventfd_write(p, f, (eventfd_t *)o, buf, len);
    case KO_UNIX: return unix_sendto(p, f, buf, len, 0, 0, 0);
    case KO_SHM: {
        if (!UOK(buf, len)) return -EFAULT;
        i64 r = shm_rw(o, f->off, (void *)(usize)buf, len, 1);
        if (r > 0) f->off += (u64)r;
        return r;
    }
    }
    return -EINVAL;
}

int kobj_poll(proc_t *p, ufile_t *f, int events) {
    kobj_t *o = f->obj;
    switch (o->kind) {
    case KO_EVENTFD: { eventfd_t *e = (eventfd_t *)o; return ((e->count ? POLLIN : 0) | (e->count < ~0ull - 1 ? POLLOUT : 0)) & events; }
    case KO_TIMERFD: { timerfd_t *t = (timerfd_t *)o; timer_update(t); return t->ticks ? POLLIN & events : 0; }
    case KO_SIGNALFD: return signalfd_ready(p, (signalfd_t *)o) ? POLLIN & events : 0;
    case KO_EPOLL: return epoll_scan(p, (epoll_t *)o, NULL, 1) ? POLLIN & events : 0;
    case KO_UNIX: return unix_poll(p, f, events);
    case KO_SHM: return events & (POLLIN | POLLOUT);
    }
    return 0;
}
