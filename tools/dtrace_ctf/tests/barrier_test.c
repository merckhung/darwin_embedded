/* Check real blocking and reuse of the Linux CTF thread barrier. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "barrier.h"

enum { THREADS = 4, ROUNDS = 1000 };
static barrier_t barrier;
static atomic_int arrivals, leaders;

static void *worker(void *unused) {
    (void)unused;
    for (int round = 0; round < ROUNDS; ++round) {
        atomic_fetch_add(&arrivals, 1);
        if (barrier_wait(&barrier)) atomic_fetch_add(&leaders, 1);
        assert(atomic_load(&arrivals) == (round + 1) * THREADS);
        barrier_wait(&barrier);
    }
    return NULL;
}

int main(void) {
    pthread_t threads[THREADS];
    barrier_init(&barrier, THREADS);
    for (int i = 0; i < THREADS; ++i) assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < THREADS; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load(&arrivals) == THREADS * ROUNDS);
    assert(atomic_load(&leaders) == ROUNDS);
    assert(pthread_barrier_destroy(&barrier) == 0);
    return 0;
}
