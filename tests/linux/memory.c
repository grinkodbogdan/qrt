/* memory.c - Firefox milestone 3 on QRT: an address space far beyond 1 GiB,
 * page protections (read-only, PROT_NONE reservations committed with mprotect,
 * no-execute, a JIT's write-then-execute), shared memory (memfd, /dev/shm,
 * MAP_SHARED|MAP_ANONYMOUS across fork), mremap, MADV_DONTNEED, many descriptors.
 * Prints "memory: ok" when everything passed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
static void kmsg(const char *s) {               /* the result in QRT's kernel log too (the test harness reads it) */
    static int fd = -2;
    if (fd == -2) fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) write(fd, s, strlen(s));
}
#define CHECK(c, what) do { char m_[160]; int ok_ = (c); if (!ok_) { snprintf(m_, sizeof m_, "memory: FAILED %s\n", what); fails++; } else snprintf(m_, sizeof m_, "memory: %s ok\n", what); fputs(m_, stdout); if (!ok_) kmsg(m_); } while (0)

static sigjmp_buf jb;
static void h_segv(int s) { siglongjmp(jb, s); }
static int faults(void (*fn)(void *), void *arg) {
    if (sigsetjmp(jb, 1)) return 1;
    fn(arg);
    return 0;
}
static void store(void *a) { *(volatile char *)a = 7; }
static void load(void *a) { (void)*(volatile char *)a; }
static void call(void *a) { ((int (*)(void))a)(); }

int main(void) {
    signal(SIGSEGV, h_segv);

    /* reserve 8 GiB (Firefox reserves gigabytes for its JIT and wasm), commit a piece far inside */
    size_t big = 8ull << 30;
    char *r = mmap(NULL, big, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    int ok = r != MAP_FAILED && (uintptr_t)r >= (1ull << 32);
    char *piece = r + (5ull << 30);
    ok = ok && faults(load, piece);                               /* PROT_NONE: no access */
    ok = ok && mprotect(piece, 1 << 20, PROT_READ | PROT_WRITE) == 0;
    if (ok) { memset(piece, 0x5a, 1 << 20); ok = piece[12345] == 0x5a; }
    /* back to PROT_NONE and to read-write: the contents are kept */
    ok = ok && mprotect(piece, 1 << 20, PROT_NONE) == 0 && faults(load, piece) && mprotect(piece, 1 << 20, PROT_READ) == 0 && piece[777] == 0x5a;
    ok = ok && faults(store, piece);                              /* read-only now */
    ok = ok && munmap(r, big) == 0;
    CHECK(ok, "8 GiB reservation, mprotect, PROT_NONE keeps contents");

    /* write-then-execute, as a JIT does: mov eax, 42; ret */
    unsigned char code[] = { 0xb8, 42, 0, 0, 0, 0xc3 };
    unsigned char *jit = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(jit, code, sizeof code);
    int nx = faults(call, jit);                                    /* data is not code */
    mprotect(jit, 4096, PROT_READ | PROT_EXEC);
    int v = ((int (*)(void))jit)();
    int wx = faults(store, jit);                                   /* code is not writable */
    CHECK(v == 42 && wx, "JIT: write, then execute");
    CHECK(nx, "no-execute data pages");

    /* memfd: two mappings of one object see the same bytes; a forked child too */
    int fd = memfd_create("qrt-test", MFD_CLOEXEC);
    ok = fd >= 0 && ftruncate(fd, 65536) == 0;
    char *m1 = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *m2 = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ok = ok && m1 != MAP_FAILED && m2 != MAP_FAILED && m1 != m2;
    if (ok) { strcpy(m1 + 4096, "shared"); ok = !strcmp(m2 + 4096, "shared"); }
    pid_t c = fork();
    if (c == 0) { strcpy(m2 + 8192, "from the child"); _exit(0); }
    waitpid(c, NULL, 0);
    ok = ok && !strcmp(m1 + 8192, "from the child");
    char buf[16] = { 0 };
    ok = ok && pread(fd, buf, 6, 4096) == 6 && !strcmp(buf, "shared");
    struct stat st;
    ok = ok && fstat(fd, &st) == 0 && st.st_size == 65536;
    CHECK(ok, "memfd + MAP_SHARED (mappings, fork, read)");

    /* MAP_SHARED|MAP_ANONYMOUS across fork */
    volatile int *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *sh = 1;
    c = fork();
    if (c == 0) { *sh = 99; _exit(0); }
    waitpid(c, NULL, 0);
    CHECK(*sh == 99, "anonymous shared memory across fork");

    /* /dev/shm (shm_open) */
    int s1 = open("/dev/shm/qrt-shm", O_RDWR | O_CREAT | O_EXCL, 0600);
    int s2 = open("/dev/shm/qrt-shm", O_RDWR);
    ok = s1 >= 0 && s2 >= 0 && ftruncate(s1, 4096) == 0;
    char *p1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s1, 0);
    char *p2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s2, 0);
    if (ok && p1 != MAP_FAILED && p2 != MAP_FAILED) { p1[10] = 'Q'; ok = p2[10] == 'Q'; } else ok = 0;
    ok = ok && unlink("/dev/shm/qrt-shm") == 0 && open("/dev/shm/qrt-shm", O_RDWR) < 0 && p2[10] == 'Q';
    CHECK(ok, "/dev/shm");

    /* mremap: grow (moving if it must), the contents come along */
    char *g = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(g + 8192, 4096);                                        /* the page after: taken by something else */
    char *blocker = mmap(g + 8192, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    strcpy(g + 5000, "keep me");
    char *g2 = mremap(g, 8192, 1 << 20, MREMAP_MAYMOVE);
    ok = g2 != MAP_FAILED && !strcmp(g2 + 5000, "keep me");
    if (ok) { g2[(1 << 20) - 1] = 1; ok = mremap(g2, 1 << 20, 4096, 0) == g2 && !strncmp(g2, "", 1); }
    CHECK(ok && blocker != MAP_FAILED, "mremap");

    /* MADV_DONTNEED: private anonymous memory reads as zero afterwards */
    char *d = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(d, 0xee, 16384);
    madvise(d, 16384, MADV_DONTNEED);
    CHECK(d[0] == 0 && d[16383] == 0, "MADV_DONTNEED");

    /* realloc of a large block (glibc moves it with mremap) */
    char *big2 = malloc(4 << 20);
    memset(big2, 3, 4 << 20);
    big2 = realloc(big2, 64 << 20);
    ok = big2 && big2[(4 << 20) - 1] == 3;
    free(big2);
    CHECK(ok, "realloc of 64 MB");

    /* MAP_32BIT and a file mapping */
    void *low = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    int ef = open("/proc/self/exe", O_RDONLY);
    if (ef < 0) ef = open("/bin/memory", O_RDONLY);
    char *img = ef >= 0 ? mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, ef, 0) : MAP_FAILED;
    CHECK(low != MAP_FAILED && (uintptr_t)low < (1ull << 31) && img != MAP_FAILED && !memcmp(img, "\x7f" "ELF", 4), "MAP_32BIT, file mapping");

    /* 200 descriptors */
    int last = -1, n = 0;
    for (int i = 0; i < 200; i++) { int x = dup(1); if (x < 0) break; last = x; n++; }
    CHECK(n == 200 && last >= 200, "200 descriptors");

    if (fails) { printf("memory: %d FAILED\n", fails); return 1; }
    printf("memory: ok\n");
    return 0;
}
