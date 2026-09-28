/* hello.c - a Linux program for QRT's Linux system-call layer.
 * Built static (glibc static-PIE and musl classic) and shipped in /bin.
 * Exercises: stdio, argv/env, uname, time, brk + mmap (malloc), files in
 * /tmp, directories, /proc, and thread-local storage. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static __thread int tls_counter = 41;

int main(int argc, char **argv) {
    struct utsname u;
    uname(&u);
    printf("Hello from a Linux program on QRT!\n");
    printf("  argv[0]=%s argc=%d  pid=%d  uid=%d\n", argv[0], argc, getpid(), getuid());
    printf("  uname: %s %s %s %s\n", u.sysname, u.nodename, u.release, u.machine);
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("  time: %s\n", when);

    char *small = malloc(1000), *big = malloc(8 << 20);       /* brk and mmap */
    memset(big, 0x5a, 8 << 20);
    long sum = 0;
    for (int i = 0; i < (8 << 20); i += 4096) sum += big[i];
    printf("  malloc: small %p, 8 MiB at %p, checksum %ld\n", (void *)small, (void *)big, sum);
    free(big);

    FILE *f = fopen("/tmp/qrt-test.txt", "w");
    fprintf(f, "written by hello at %ld\n", (long)now);
    fclose(f);
    char line[80] = "";
    f = fopen("/tmp/qrt-test.txt", "r");
    if (f && fgets(line, sizeof line, f)) printf("  file round trip: %s", line);
    if (f) fclose(f);

    DIR *d = opendir("/");
    printf("  / contains:");
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') printf(" %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
    printf("\n");
    if (d) closedir(d);

    f = fopen("/proc/meminfo", "r");
    if (f && fgets(line, sizeof line, f)) printf("  /proc/meminfo: %s", line);
    if (f) fclose(f);

    tls_counter++;
    printf("  thread-local storage: %d (expected 42)\n", tls_counter);
    return 0;
}
