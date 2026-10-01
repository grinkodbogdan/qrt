/* threads.c - pthreads on QRT: a dynamically linked glibc program that
 * starts worker threads (clone), protects a counter with a mutex, signals
 * with a condition variable and joins them (futex), with per-thread TLS.
 * Prints "threads: ok" on success. */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define N 4
#define ROUNDS 20000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static int ready;
static __thread int mine;

static void *worker(void *arg) {
    long id = (long)arg;
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cond, &lock);   /* everyone starts together */
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
        mine++;
        if (i % 5000 == 0) sched_yield();
    }
    return (void *)(id * 100 + (mine == ROUNDS));
}

int main(void) {
    pthread_t t[N];
    for (long i = 0; i < N; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    usleep(20000);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    int good = 1;
    for (long i = 0; i < N; i++) {
        void *r;
        pthread_join(t[i], &r);
        if ((long)r != i * 100 + 1) good = 0;
    }
    printf("threads: %d threads, counter %ld (expected %d), TLS %s\n", N, counter, N * ROUNDS, good ? "private" : "WRONG");
    printf("threads: %s\n", good && counter == N * ROUNDS ? "ok" : "FAILED");
    return good && counter == N * ROUNDS ? 0 : 1;
}
