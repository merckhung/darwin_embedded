/* Guest probe; run after native kernel and pthread initialization. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t key;
static int once_calls;
static int destructors;
static int stage;
static int release_worker;
static int worker_error;
static int main_value;
static int worker_value;

static void
once_routine(void)
{
    ++once_calls;
}

static void
key_destructor(void *value)
{
    if (value == &worker_value)
        ++destructors;
    else
        worker_error = 1;
}

static void *
worker(void *argument)
{
    errno = 73;
    if (pthread_setspecific(key, argument) || pthread_once(&once, once_routine))
        worker_error = 2;
    if (pthread_mutex_lock(&lock))
        return 0;
    stage = 1;
    if (pthread_cond_signal(&condition))
        worker_error = 3;
    while (!release_worker) {
        if (pthread_cond_wait(&condition, &lock)) {
            worker_error = 4;
            break;
        }
    }
    if (errno != 73 || pthread_getspecific(key) != argument)
        worker_error = 5;
    if (pthread_mutex_unlock(&lock))
        worker_error = 6;
    return argument;
}

static void
unlock_after_cancel(void *argument)
{
    if (pthread_mutex_unlock(argument))
        worker_error = 7;
}

static void *
cancel_worker(void *argument)
{
    if (pthread_setspecific(key, argument) || pthread_mutex_lock(&lock))
        return 0;
    pthread_cleanup_push(unlock_after_cancel, &lock);
    stage = 2;
    if (pthread_cond_signal(&condition))
        worker_error = 8;
    for (;;) {
        if (pthread_cond_wait(&condition, &lock)) {
            worker_error = 9;
            break;
        }
    }
    pthread_cleanup_pop(1);
    return 0;
}

int
native_pthread_probe(void)
{
    pthread_t thread;
    void *result = 0;
    if (!pthread_main_np() || pthread_jit_write_protect_supported_np())
        return 1;
    if (pthread_key_create(&key, key_destructor) ||
        pthread_setspecific(key, &main_value) || pthread_once(&once, once_routine))
        return 2;
    errno = 41;
    if (pthread_mutex_lock(&lock) || pthread_create(&thread, 0, worker, &worker_value))
        return 3;
    while (stage != 1) {
        if (pthread_cond_wait(&condition, &lock))
            return 4;
    }
    if (errno != 41 || pthread_getspecific(key) != &main_value || once_calls != 1)
        return 5;
    release_worker = 1;
    if (pthread_cond_signal(&condition) || pthread_mutex_unlock(&lock) ||
        pthread_join(thread, &result))
        return 6;
    if (result != &worker_value || destructors != 1 || worker_error)
        return 7;
    if (pthread_mutex_lock(&lock) ||
        pthread_create(&thread, 0, cancel_worker, &worker_value))
        return 8;
    while (stage != 2) {
        if (pthread_cond_wait(&condition, &lock))
            return 9;
    }
    if (pthread_mutex_unlock(&lock) || pthread_cancel(thread) ||
        pthread_join(thread, &result))
        return 10;
    if (result != PTHREAD_CANCELED || destructors != 2 || worker_error)
        return 11;
    if (pthread_mutex_trylock(&lock) || pthread_mutex_unlock(&lock) ||
        pthread_key_delete(key) || pthread_cond_destroy(&condition) ||
        pthread_mutex_destroy(&lock))
        return 12;
    return 0;
}
