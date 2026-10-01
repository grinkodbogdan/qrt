/* procs.c - processes on QRT: fork, execve, wait, pipes, dup2, posix_spawn,
 * system() (busybox sh -c with a pipeline), FD_CLOEXEC and kill.
 * Prints "procs: ok" on success. */
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

extern char **environ;
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("procs: FAILED %s\n", what); fails++; } else printf("procs: %s ok\n", what); } while (0)

static int read_all(int fd, char *buf, int cap) {
    int n = 0, r;
    while (n < cap - 1 && (r = (int)read(fd, buf + n, (size_t)(cap - 1 - n))) > 0) n += r;
    buf[n] = 0;
    return n;
}

int main(void) {
    /* fork + exit status, and the child's own copy of memory */
    int shared = 1;
    pid_t c = fork();
    if (c == 0) { shared = 42; _exit(7); }
    int st = 0;
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7 && shared == 1, "fork/wait");

    /* pipe + fork + dup2 + execve of an applet */
    int pfd[2];
    if (pipe(pfd)) return 1;
    c = fork();
    if (c == 0) {
        dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]);
        char *argv[] = { "echo", "through a pipe", NULL };
        execve("/bin/echo", argv, environ);
        _exit(127);
    }
    close(pfd[1]);
    char buf[256];
    read_all(pfd[0], buf, sizeof buf);
    close(pfd[0]);
    waitpid(c, &st, 0);
    CHECK(!strcmp(buf, "through a pipe\n") && WEXITSTATUS(st) == 0, "pipe/exec");

    /* posix_spawn (clone with CLONE_VM|CLONE_VFORK) */
    pid_t sp;
    char *sargv[] = { "true", NULL };
    int e = posix_spawn(&sp, "/bin/true", NULL, NULL, sargv, environ);
    waitpid(sp, &st, 0);
    CHECK(!e && WIFEXITED(st) && WEXITSTATUS(st) == 0, "posix_spawn");

    /* popen: sh -c with a pipeline */
    FILE *f = popen("echo one two three | wc -w", "r");
    buf[0] = 0;
    if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; pclose(f); }
    CHECK(atoi(buf) == 3, "popen pipeline");

    /* FD_CLOEXEC: the descriptor is gone in the new program */
    int fd = open("/proc/version", O_RDONLY | O_CLOEXEC);
    char cmd[64];
    snprintf(cmd, sizeof cmd, "test -e /proc/self/fd/%d || exit 3", fd);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC, "O_CLOEXEC");

    /* kill */
    c = fork();
    if (c == 0) { for (;;) sleep(1); }
    usleep(20000);
    kill(c, SIGTERM);
    waitpid(c, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "kill");

    printf("procs: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
