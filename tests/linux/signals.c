/* signals.c - POSIX signals on QRT: handlers with and without SA_SIGINFO, masks,
 * pending sets, faults turned into SIGSEGV/SIGFPE (and siglongjmp out of them),
 * the alternate stack, timers, SA_RESTART, sigsuspend, sigtimedwait, SIGCHLD,
 * SIGPIPE, default actions, thread-directed signals, FPU state across a handler.
 * Prints "signals: ok" when everything passed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
static void kmsg(const char *s) {               /* the result in QRT's kernel log too (the test harness reads it) */
    static int fd = -2;
    if (fd == -2) fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) write(fd, s, strlen(s));
}
#define CHECK(c, what) do { char m_[160]; int ok_ = (c); if (!ok_) { snprintf(m_, sizeof m_, "signals: FAILED %s\n", what); fails++; } else snprintf(m_, sizeof m_, "signals: %s ok\n", what); fputs(m_, stdout); if (!ok_) kmsg(m_); } while (0)

static volatile sig_atomic_t got_usr1, got_usr2, got_alrm, got_chld, got_info_ok, on_alt;
static volatile void *fault_addr;
static sigjmp_buf jb;
static char altstack[65536];

static void h_usr1(int s) { (void)s; got_usr1++; }
static void h_usr2(int s) { (void)s; got_usr2++; }
static void h_alrm(int s) { (void)s; got_alrm++; }
static void h_chld(int s) { (void)s; got_chld++; }
static void h_info(int s, siginfo_t *si, void *uc) {
    (void)uc;
    got_info_ok = s == SIGUSR1 && si->si_pid == getpid() && si->si_code == SI_USER;
}
static void h_fault(int s, siginfo_t *si, void *uc) {
    (void)s; (void)uc;
    fault_addr = si->si_addr;
    char here;
    on_alt = &here >= altstack && &here < altstack + sizeof altstack;
    siglongjmp(jb, s);
}
static void h_fpu(int s) {
    (void)s;
    volatile double x = 1.0;
    for (int i = 0; i < 100; i++) x = x * 1.37 + 0.5;          /* scribble on the SSE registers */
    got_usr2 += x > 0;
}

static void set(int sig, void (*h)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}
static void set_info(int sig, void (*h)(int, siginfo_t *, void *), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void *thread_wait(void *arg) {
    (void)arg;
    for (int i = 0; i < 200 && !got_usr2; i++) usleep(5000);
    return NULL;
}

int main(void) {
    /* a handler, kill() to ourselves */
    set(SIGUSR1, h_usr1, 0);
    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == 1, "handler");

    /* SA_SIGINFO: the sender and the code */
    set_info(SIGUSR1, h_info, 0);
    kill(getpid(), SIGUSR1);
    CHECK(got_info_ok, "siginfo");

    /* raise() = tgkill to this thread */
    set(SIGUSR2, h_usr2, 0);
    raise(SIGUSR2);
    CHECK(got_usr2 == 1, "raise/tgkill");

    /* blocked: pending until unblocked */
    set(SIGUSR1, h_usr1, 0);
    got_usr1 = 0;
    sigset_t m, old, pend;
    sigemptyset(&m); sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, &old);
    kill(getpid(), SIGUSR1);
    sigpending(&pend);
    int was_pending = sigismember(&pend, SIGUSR1) && !got_usr1;
    sigprocmask(SIG_SETMASK, &old, NULL);
    CHECK(was_pending && got_usr1 == 1, "mask/pending");

    /* a store to a read-only page: SIGSEGV with the address, on the alternate stack */
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    set_info(SIGSEGV, h_fault, SA_ONSTACK);
    char *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int sig = sigsetjmp(jb, 1);
    if (!sig) { ro[100] = 1; }
    CHECK(sig == SIGSEGV && fault_addr == ro + 100 && on_alt, "SIGSEGV (read-only page, sigaltstack)");

    /* PROT_NONE and NULL */
    char *none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    sig = sigsetjmp(jb, 1);
    if (!sig) { (void)*(volatile char *)none; }
    int ok1 = sig == SIGSEGV && fault_addr == none;
    sig = sigsetjmp(jb, 1);
    if (!sig) { (void)*(volatile int *)(uintptr_t)8; }
    CHECK(ok1 && sig == SIGSEGV && fault_addr == (void *)8, "SIGSEGV (PROT_NONE, NULL)");

    /* integer division by zero */
    set_info(SIGFPE, h_fault, 0);
    sig = sigsetjmp(jb, 1);
    volatile int zero = 0, r = 1;
    if (!sig) { r = 10 / zero; }
    CHECK(sig == SIGFPE && r == 1, "SIGFPE");

    /* the FPU/SSE state survives a handler */
    set(SIGUSR2, h_fpu, 0);
    volatile double a = 3.25, b = 0;
    double c = a * 2;
    raise(SIGUSR2);
    b = c + a;
    CHECK(b == 9.75, "FPU state across a handler");

    /* setitimer + pause */
    set(SIGALRM, h_alrm, 0);
    struct itimerval it = { { 0, 0 }, { 0, 50000 } };
    setitimer(ITIMER_REAL, &it, NULL);
    pause();
    CHECK(got_alrm == 1, "setitimer/pause");
    alarm(1);
    unsigned left = alarm(0);
    CHECK(left == 1, "alarm");

    /* SA_RESTART: a read() interrupted by a handler carries on */
    int pfd[2];
    pipe(pfd);
    set(SIGUSR1, h_usr1, SA_RESTART);
    got_usr1 = 0;
    pid_t parent = getpid(), c1 = fork();
    if (c1 == 0) { usleep(80000); kill(parent, SIGUSR1); usleep(80000); write(pfd[1], "x", 1); _exit(0); }
    char ch = 0;
    ssize_t n = read(pfd[0], &ch, 1);
    waitpid(c1, NULL, 0);
    CHECK(n == 1 && ch == 'x' && got_usr1 == 1, "SA_RESTART");

    /* without it: EINTR */
    set(SIGUSR1, h_usr1, 0);
    got_usr1 = 0;
    c1 = fork();
    if (c1 == 0) { usleep(80000); kill(parent, SIGUSR1); _exit(0); }
    errno = 0;
    n = read(pfd[0], &ch, 1);
    int e = errno;
    waitpid(c1, NULL, 0);
    CHECK(n == -1 && e == EINTR && got_usr1 == 1, "EINTR");

    /* sigsuspend: atomically unblock and wait */
    got_usr1 = 0;
    sigprocmask(SIG_BLOCK, &m, &old);
    c1 = fork();
    if (c1 == 0) { usleep(50000); kill(parent, SIGUSR1); _exit(0); }
    sigset_t none_set;
    sigemptyset(&none_set);
    sigsuspend(&none_set);
    sigset_t now;
    sigprocmask(SIG_SETMASK, NULL, &now);
    waitpid(c1, NULL, 0);
    CHECK(got_usr1 == 1 && sigismember(&now, SIGUSR1), "sigsuspend");
    sigprocmask(SIG_SETMASK, &old, NULL);

    /* sigtimedwait: take a blocked signal without a handler running */
    sigset_t w;
    sigemptyset(&w); sigaddset(&w, SIGUSR2);
    sigprocmask(SIG_BLOCK, &w, &old);
    got_usr2 = 0;
    kill(getpid(), SIGUSR2);
    siginfo_t si;
    struct timespec ts = { 1, 0 };
    int s2 = sigtimedwait(&w, &si, &ts);
    sigprocmask(SIG_SETMASK, &old, NULL);
    CHECK(s2 == SIGUSR2 && si.si_pid == getpid() && got_usr2 == 0, "sigtimedwait");

    /* SIGCHLD */
    set(SIGCHLD, h_chld, 0);
    c1 = fork();
    if (c1 == 0) _exit(3);
    int st = 0;
    while (waitpid(c1, &st, 0) < 0 && errno == EINTR) {}
    for (int i = 0; i < 50 && !got_chld; i++) usleep(2000);
    CHECK(got_chld >= 1 && WIFEXITED(st) && WEXITSTATUS(st) == 3, "SIGCHLD");
    set(SIGCHLD, SIG_DFL, 0);

    /* SIGPIPE: ignored -> EPIPE; default -> the writer dies of it */
    int q[2];
    pipe(q);
    close(q[0]);
    set(SIGPIPE, SIG_IGN, 0);
    errno = 0;
    n = write(q[1], "y", 1);
    int pipe_ok = n == -1 && errno == EPIPE;
    set(SIGPIPE, SIG_DFL, 0);
    c1 = fork();
    if (c1 == 0) { write(q[1], "y", 1); _exit(0); }
    waitpid(c1, &st, 0);
    close(q[1]);
    CHECK(pipe_ok && WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "SIGPIPE");

    /* default action: SIGTERM ends a child, the parent sees which signal */
    c1 = fork();
    if (c1 == 0) { for (;;) pause(); }
    usleep(30000);
    kill(c1, SIGTERM);
    waitpid(c1, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "default action");

    /* a signal to another thread */
    set(SIGUSR2, h_usr2, 0);
    got_usr2 = 0;
    pthread_t t;
    pthread_create(&t, NULL, thread_wait, NULL);
    usleep(20000);
    pthread_kill(t, SIGUSR2);
    pthread_join(t, NULL);
    CHECK(got_usr2 == 1, "pthread_kill");

    if (fails) { printf("signals: %d FAILED\n", fails); return 1; }
    printf("signals: ok\n");
    return 0;
}
