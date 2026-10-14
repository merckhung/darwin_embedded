/* Run after native_tlv_init and actual pthread/malloc initialization. */
#include <pthread.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>

static int events[64];
static unsigned event_count;
static int overflow;
static int reentrant = 91;
static int main_sentinel = 102;
static int worker_values[] = {201, 202, 203};

static void record(void *argument)
{
    int value = *(int *)argument;
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = value;
    else
        overflow = 1;
    if (value == 13)
        _tlv_atexit(record, &reentrant);
}

static void *worker(void *argument)
{
    (void)argument;
    for (unsigned n = 0; n < sizeof(worker_values) / sizeof(worker_values[0]); ++n)
        _tlv_atexit(record, worker_values + n);
    return &worker_values;
}

int native_tlv_check(void)
{
    int values[17];
    /* Three upstream seven-entry list nodes, with a reentrant destructor. */
    for (int n = 0; n < 17; ++n) {
        values[n] = n;
        _tlv_atexit(record, values + n);
    }
    _tlv_atexit(0, 0);
    _tlv_exit();
    if (overflow || event_count != 18)
        return 1;
    unsigned index = 0;
    for (int n = 16; n >= 0; --n) {
        if (events[index++] != n)
            return 2;
        if (n == 13 && events[index++] != 91)
            return 3;
    }
    _tlv_exit();
    if (event_count != 18)
        return 4;
    event_count = 0;
    _tlv_atexit(record, &main_sentinel);
    pthread_t thread;
    void *result = 0;
    if (pthread_create(&thread, 0, worker, 0) || pthread_join(thread, &result))
        return 5;
    if (result != &worker_values || event_count != 3 ||
        events[0] != 203 || events[1] != 202 || events[2] != 201)
        return 6;
    /* Thread exit must not consume the main thread's pending destructor. */
    _tlv_exit();
    if (overflow || event_count != 4 || events[3] != main_sentinel)
        return 7;
    _tlv_exit();
    return event_count != 4 ? 8 : 0;
}
