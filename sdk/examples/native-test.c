/* native-test.c - a native QRT program (built with qrt-cc, musl on QRT's own system
 * calls) exercising the C library: stdio, memory, files, directories, time, threads,
 * fork/exec/wait, pipes, signals.  Prints "native: ok". */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what) {
    printf("native: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        fails++;
        int fd = open("/dev/kmsg", O_WRONLY);
        if (fd >= 0) { dprintf(fd, "native: %s FAILED\n", what); close(fd); }
    }
}

static int counter;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
    for (int i = 0; i < 10000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    return arg;
}
static volatile sig_atomic_t got;
static void on_usr1(int s) { (void)s; got = 1; }

int main(void) {
    struct utsname u;
    check(uname(&u) == 0 && !strcmp(u.sysname, "QRT"), "uname says QRT (the native personality)");

    /* syscall() takes Linux's numbers (source compatibility) and the C library translates */
    check(syscall(SYS_getpid) == getpid() && syscall(SYS_gettid) > 0 && syscall(99999) == -1 && errno == ENOSYS, "syscall() with Linux numbers");

    char *m = malloc(1 << 20);
    memset(m, 7, 1 << 20);
    void *big = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m && m[12345] == 7 && big != MAP_FAILED, "malloc, mmap");
    free(m);

    FILE *f = fopen("/tmp/native.txt", "w");
    fprintf(f, "written by a native program %d\n", 42);
    fclose(f);
    char line[64] = { 0 };
    f = fopen("/tmp/native.txt", "r");
    fgets(line, sizeof line, f);
    fclose(f);
    check(!strcmp(line, "written by a native program 42\n") && unlink("/tmp/native.txt") == 0, "files");

    DIR *d = opendir("/bin");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) n++;
    if (d) closedir(d);
    check(n > 5, "directories");

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    usleep(20000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(ms >= 19 && ms < 200 && time(NULL) > 1600000000, "time");

    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    check(counter == 40000, "threads");

    int p[2];
    pipe(p);
    pid_t c = fork();
    if (c == 0) {
        dup2(p[1], 1);
        close(p[0]); close(p[1]);
        execl("/bin/echo", "echo", "child of a native program", (char *)0);
        _exit(127);
    }
    close(p[1]);
    char buf[64] = { 0 };
    int r = (int)read(p[0], buf, sizeof buf - 1);
    int st = 0;
    waitpid(c, &st, 0);
    check(r > 0 && !strcmp(buf, "child of a native program\n") && WIFEXITED(st) && !WEXITSTATUS(st), "fork, exec, pipe, wait");

    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    check(got == 1, "signals");

    if (fails) { printf("native: %d FAILED\n", fails); return 1; }
    printf("native: ok\n");
    return 0;
}
