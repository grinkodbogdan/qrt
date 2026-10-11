/* cores.c - do a program's threads run at the same time?  The same fixed amount of
 * arithmetic, on 1 thread and then split over N (default: every core the kernel
 * reports); prints both times and the speed-up.  Not a pass/fail test on its own: on
 * one core the speed-up is ~1x, on eight ~ the number of free cores. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define WORK 400000000ull

static unsigned long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000;
}

static void *spin(void *arg) {
    unsigned long long n = *(unsigned long long *)arg, x = 1;
    for (unsigned long long i = 0; i < n; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
    *(unsigned long long *)arg = x;
    return NULL;
}

static unsigned long long run(int threads) {
    pthread_t t[64];
    unsigned long long share[64];
    unsigned long long t0 = now_ms();
    for (int i = 0; i < threads; i++) { share[i] = WORK / (unsigned long long)threads; pthread_create(&t[i], NULL, spin, &share[i]); }
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    return now_ms() - t0;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    unsigned long long one = run(1), many = run((int)n);
    printf("cores: 1 thread %llu ms, %ld threads %llu ms: %llu.%llux\n", one, n, many,
           many ? one / many : 0, many ? one * 10 / many % 10 : 0);
    return 0;
}
