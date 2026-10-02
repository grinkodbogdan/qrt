/* events.c - Firefox milestone 4 on QRT: eventfd, timerfd, signalfd, epoll, and
 * Unix sockets - socketpair (stream, datagram, seqpacket), a named server with
 * listen/accept and SO_PEERCRED, an abstract name, descriptors passed with
 * SCM_RIGHTS, hang-up seen through epoll.  Prints "events: ok" when everything passed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void kmsg(const char *s) {               /* the result in QRT's kernel log too (the test harness reads it) */
    static int fd = -2;
    if (fd == -2) fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) write(fd, s, strlen(s));
}
#define CHECK(c, what) do { char m_[160]; int ok_ = (c); if (!ok_) { snprintf(m_, sizeof m_, "events: FAILED %s\n", what); fails++; } else snprintf(m_, sizeof m_, "events: %s ok\n", what); fputs(m_, stdout); if (!ok_) kmsg(m_); } while (0)

static int send_fd(int sock, int fd, const char *msg) {
    struct iovec iov = { (void *)msg, strlen(msg) };
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = ctl; mh.msg_controllen = sizeof ctl;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return (int)sendmsg(sock, &mh, 0);
}

static int recv_fd(int sock, char *buf, int cap, int *fd) {
    struct iovec iov = { buf, (size_t)cap - 1 };
    char ctl[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = ctl; mh.msg_controllen = sizeof ctl;
    int n = (int)recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    if (n < 0) return n;
    buf[n] = 0;
    *fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof *fd);
    return n;
}

int main(void) {
    /* eventfd: counter, semaphore mode, non-blocking */
    int ev = eventfd(0, EFD_NONBLOCK);
    uint64_t v = 0;
    int ok = read(ev, &v, 8) == -1 && errno == EAGAIN;
    v = 3; write(ev, &v, 8); v = 4; write(ev, &v, 8);
    ok = ok && read(ev, &v, 8) == 8 && v == 7;
    int sem = eventfd(2, EFD_SEMAPHORE);
    uint64_t a = 0, b = 0;
    ok = ok && read(sem, &a, 8) == 8 && read(sem, &b, 8) == 8 && a == 1 && b == 1;
    CHECK(ok, "eventfd");

    /* timerfd: a 20 ms period */
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = { { 0, 20000000 }, { 0, 20000000 } };
    timerfd_settime(tf, 0, &its, NULL);
    usleep(75000);
    uint64_t ticks = 0;
    ok = read(tf, &ticks, 8) == 8 && ticks >= 3 && ticks <= 5;
    struct itimerspec cur;
    timerfd_gettime(tf, &cur);
    CHECK(ok && cur.it_interval.tv_nsec == 20000000, "timerfd");

    /* signalfd */
    sigset_t m;
    sigemptyset(&m); sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, NULL);
    int sfd = signalfd(-1, &m, SFD_NONBLOCK);
    kill(getpid(), SIGUSR1);
    struct signalfd_siginfo si;
    ok = read(sfd, &si, sizeof si) == sizeof si && si.ssi_signo == SIGUSR1 && si.ssi_pid == (uint32_t)getpid();
    ok = ok && read(sfd, &si, sizeof si) == -1 && errno == EAGAIN;
    CHECK(ok, "signalfd");

    /* epoll over an eventfd, a pipe and the timer; one-shot; delete */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int pfd[2];
    pipe(pfd);
    struct epoll_event e1 = { EPOLLIN, { .u64 = 111 } }, e2 = { EPOLLIN | EPOLLONESHOT, { .u64 = 222 } }, e3 = { EPOLLIN, { .u64 = 333 } };
    epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e1);
    epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &e2);
    epoll_ctl(ep, EPOLL_CTL_ADD, tf, &e3);
    struct epoll_event out[8];
    int n = epoll_wait(ep, out, 8, 0);                             /* nothing yet but maybe the timer */
    int timer_only = 1;
    for (int i = 0; i < n; i++) if (out[i].data.u64 != 333) timer_only = 0;
    v = 1; write(ev, &v, 8);
    write(pfd[1], "p", 1);
    n = epoll_wait(ep, out, 8, 1000);
    int seen1 = 0, seen2 = 0;
    for (int i = 0; i < n; i++) { seen1 |= out[i].data.u64 == 111; seen2 |= out[i].data.u64 == 222; }
    int again = epoll_wait(ep, out, 8, 0), seen2b = 0;             /* one-shot: the pipe is not reported twice */
    for (int i = 0; i < again; i++) seen2b |= out[i].data.u64 == 222;
    ok = timer_only && seen1 && seen2 && !seen2b && epoll_ctl(ep, EPOLL_CTL_DEL, ev, NULL) == 0 && epoll_ctl(ep, EPOLL_CTL_DEL, ev, NULL) == -1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    epoll_ctl(ep, EPOLL_CTL_DEL, tf, NULL);
    n = epoll_wait(ep, out, 8, 60);                                /* nothing left: a timeout */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ok && n == 0 && ms >= 50, "epoll");

    /* socketpair: a stream, both ways, then descriptors over it */
    int sp[2];
    ok = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0;
    char buf[64];
    ok = ok && write(sp[0], "ping", 4) == 4 && read(sp[1], buf, sizeof buf) == 4 && !memcmp(buf, "ping", 4);
    ok = ok && send(sp[1], "pong", 4, 0) == 4 && recv(sp[0], buf, sizeof buf, 0) == 4 && !memcmp(buf, "pong", 4);
    int q[2];
    pipe(q);
    ok = ok && send_fd(sp[0], q[1], "here is a pipe") > 0;
    int got = -1;
    ok = ok && recv_fd(sp[1], buf, sizeof buf, &got) > 0 && !strcmp(buf, "here is a pipe") && got >= 0 && got != q[1];
    close(q[1]);                                                   /* the received copy keeps the pipe open */
    ok = ok && write(got, "via fd", 6) == 6 && read(q[0], buf, sizeof buf) == 6 && !memcmp(buf, "via fd", 6);
    ok = ok && (fcntl(got, F_GETFD) & FD_CLOEXEC);
    CHECK(ok, "socketpair + SCM_RIGHTS");

    /* datagrams and seqpackets keep their boundaries */
    int dg[2], sq[2];
    ok = socketpair(AF_UNIX, SOCK_DGRAM, 0, dg) == 0 && socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sq) == 0;
    send(dg[0], "one", 3, 0); send(dg[0], "three", 5, 0);
    ok = ok && recv(dg[1], buf, sizeof buf, 0) == 3 && recv(dg[1], buf, sizeof buf, 0) == 5;
    send(sq[0], "abcdef", 6, 0);
    ok = ok && recv(sq[1], buf, 2, 0) == 2;                        /* the rest of the record is dropped */
    ok = ok && recv(sq[1], buf, sizeof buf, MSG_DONTWAIT) == -1 && errno == EAGAIN;
    CHECK(ok, "SOCK_DGRAM, SOCK_SEQPACKET");

    /* a named server: bind, listen, a child connects; SO_PEERCRED says who */
    unlink("/tmp/qrt.sock");
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, "/tmp/qrt.sock");
    ok = bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(srv, 4) == 0;
    ok = ok && access("/tmp/qrt.sock", F_OK) == 0;
    fcntl(srv, F_SETFL, O_NONBLOCK);
    ok = ok && accept(srv, NULL, NULL) == -1 && errno == EAGAIN;
    fcntl(srv, F_SETFL, 0);
    pid_t c = fork();
    if (c == 0) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&addr, sizeof addr)) _exit(2);
        write(s, "hello server", 12);
        char r[8];
        int k = (int)read(s, r, sizeof r);
        _exit(k == 2 && !memcmp(r, "ok", 2) ? 0 : 3);
    }
    int cl = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
    n = cl >= 0 ? (int)read(cl, buf, sizeof buf) : -1;
    struct ucred cred;
    socklen_t cl_len = sizeof cred;
    ok = ok && n == 12 && !memcmp(buf, "hello server", 12) &&
         getsockopt(cl, SOL_SOCKET, SO_PEERCRED, &cred, &cl_len) == 0 && cred.pid == c;
    write(cl, "ok", 2);
    int st = 0;
    waitpid(c, &st, 0);
    CHECK(ok && WIFEXITED(st) && WEXITSTATUS(st) == 0, "bind/listen/accept/connect, SO_PEERCRED");

    /* the client went away: epoll reports the hang-up, read returns 0 */
    int ep2 = epoll_create1(0);
    struct epoll_event eh = { EPOLLIN | EPOLLRDHUP, { .fd = cl } };
    epoll_ctl(ep2, EPOLL_CTL_ADD, cl, &eh);
    n = epoll_wait(ep2, out, 8, 500);
    ok = n == 1 && (out[0].events & (EPOLLRDHUP | EPOLLHUP)) && read(cl, buf, sizeof buf) == 0;
    CHECK(ok, "hang-up");
    unlink("/tmp/qrt.sock");
    ok = access("/tmp/qrt.sock", F_OK) != 0;

    /* an abstract name, and poll() on a socket */
    int as = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ab = { .sun_family = AF_UNIX };
    memcpy(ab.sun_path, "\0qrt-abstract", 13);
    socklen_t alen = (socklen_t)(sizeof(sa_family_t) + 13);
    ok = ok && bind(as, (struct sockaddr *)&ab, alen) == 0 && listen(as, 1) == 0;
    int cs = socket(AF_UNIX, SOCK_STREAM, 0);
    ok = ok && connect(cs, (struct sockaddr *)&ab, alen) == 0;
    struct pollfd pf = { as, POLLIN, 0 };
    ok = ok && poll(&pf, 1, 500) == 1 && (pf.revents & POLLIN);
    CHECK(ok, "abstract name, unlink, poll");

    if (fails) { printf("events: %d FAILED\n", fails); return 1; }
    printf("events: ok\n");
    return 0;
}
