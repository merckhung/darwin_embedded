/* Run after native_tls_init, linked with the genuine compiler TLS DLL. */
#include <pthread.h>
#include <stdint.h>
#include <dlfcn.h>
#include <mach-o/dyld_priv.h>

extern uint64_t native_tls_module_read(void);
extern uint64_t native_tls_module_zero(void);
extern void native_tls_module_write(uint64_t);
extern void *native_tls_module_address(void);
extern int native_tls_module_ready(void);
extern void native_tls_module_destructor(void (*)(void*), int);
extern uint64_t native_tls_zero_read(void);
extern void native_tls_zero_write(uint64_t);
extern void *native_tls_zero_address(void);
extern int native_tls_zero_ready(void);

static _Thread_local uint64_t initialized = UINT64_C(0xa5a55a5af00dcafe);
static __thread uint64_t zero_fill[5];
static _Thread_local int lifetime;
static int destructor_values[8];
static unsigned destructor_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned arrived;
static unsigned released;
static void destroy(void *pointer);
struct worker_state { unsigned id; int failure; uintptr_t local_address; uintptr_t module_address; uintptr_t zero_address; };

struct dynamic_functions {
    uint64_t (*read)(void);
    uint64_t (*zero)(void);
    void (*write)(uint64_t);
    void *(*address)(void);
    int (*ready)(void);
    void (*destructor)(void (*)(void*), int);
};
static struct dynamic_functions dynamic;

static void *dynamic_worker(void *argument)
{
    struct worker_state* state = argument;
    if (!dynamic.ready() || dynamic.read() != UINT64_C(0x13579bdf2468ace0) || dynamic.zero())
        state->failure = 1;
    dynamic.write(701);
    dynamic.destructor(destroy, 31);
    state->module_address = (uintptr_t)dynamic.address();
    pthread_mutex_lock(&lock);
    ++arrived;
    pthread_cond_broadcast(&condition);
    while (!released)
        pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    /* The main thread has dlclosed the TLS image while we retained live TLS. */
    if (dynamic.read() != 701 || dynamic.zero() != 702)
        state->failure = 2;
    return argument;
}

static void destroy(void *pointer)
{
    pthread_mutex_lock(&lock);
    if (destructor_count < sizeof(destructor_values) / sizeof(destructor_values[0]))
        destructor_values[destructor_count++] = *(int*)pointer;
    pthread_mutex_unlock(&lock);
}

static void *worker(void *pointer)
{
    struct worker_state* state = pointer;
    if (initialized != UINT64_C(0xa5a55a5af00dcafe) || zero_fill[0] || zero_fill[4] ||
        native_tls_module_read() != UINT64_C(0x13579bdf2468ace0) || native_tls_module_zero() ||
        native_tls_zero_read())
        state->failure = 1;
    initialized = state->id + 200;
    zero_fill[4] = state->id + 300;
    lifetime = state->id + 10;
    _tlv_atexit(destroy, &lifetime);
    native_tls_module_write(state->id + 400);
    native_tls_module_destructor(destroy, state->id + 20);
    native_tls_zero_write(state->id + 500);
    state->local_address = (uintptr_t)&initialized;
    state->module_address = (uintptr_t)native_tls_module_address();
    state->zero_address = (uintptr_t)native_tls_zero_address();
    pthread_mutex_lock(&lock);
    ++arrived;
    pthread_cond_broadcast(&condition);
    while (!released)
        pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    if (initialized != state->id + 200 || zero_fill[4] != state->id + 300 ||
        native_tls_module_read() != state->id + 400 || native_tls_module_zero() != state->id + 401 ||
        native_tls_zero_read() != state->id + 500)
        state->failure = 2;
    return pointer;
}

static int dynamic_check(void)
{
    void *handle = dlopen("/usr/lib/libnative_tls_dynamic.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        return 20;
    dlerror();
    dynamic.read = (uint64_t (*)(void))dlsym(handle, "native_tls_module_read");
    dynamic.zero = (uint64_t (*)(void))dlsym(handle, "native_tls_module_zero");
    dynamic.write = (void (*)(uint64_t))dlsym(handle, "native_tls_module_write");
    dynamic.address = (void *(*)(void))dlsym(handle, "native_tls_module_address");
    dynamic.ready = (int (*)(void))dlsym(handle, "native_tls_module_ready");
    dynamic.destructor = (void (*)(void (*)(void*), int))dlsym(handle, "native_tls_module_destructor");
    if (dlerror() || !dynamic.read || !dynamic.zero || !dynamic.write ||
        !dynamic.address || !dynamic.ready || !dynamic.destructor)
        return 21;
    if (!dynamic.ready() || dynamic.read() != UINT64_C(0x13579bdf2468ace0) || dynamic.zero() ||
        dynamic.address() == native_tls_module_address())
        return 22;
    dynamic.write(601);
    arrived = 0;
    released = 0;
    struct worker_state state = {.id = 3};
    pthread_t thread;
    if (pthread_create(&thread, 0, dynamic_worker, &state))
        return 23;
    pthread_mutex_lock(&lock);
    while (!arrived)
        pthread_cond_wait(&condition, &lock);
    int same_address = state.module_address == (uintptr_t)dynamic.address();
    pthread_mutex_unlock(&lock);
    /* Apple keeps MH_HAS_TLV_DESCRIPTORS images and dependencies mapped. */
    int closed = dlclose(handle);
    pthread_mutex_lock(&lock);
    released = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    void *result = 0;
    if (pthread_join(thread, &result) || result != &state || state.failure || closed || same_address)
        return 24;
    if (destructor_count != 5 || destructor_values[4] != 31 ||
        dynamic.read() != 601 || dynamic.zero() != 602)
        return 25;
    void *again = dlopen("/usr/lib/libnative_tls_dynamic.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!again)
        return 26;
    void *read_again = dlsym(again, "native_tls_module_read");
    if (read_again != (void*)dynamic.read || dynamic.read() != 601 || dlclose(again))
        return 27;
    return 0;
}

int native_tls_check(void)
{
    if (!native_tls_module_ready() || !native_tls_zero_ready() || native_tls_zero_read() ||
        initialized != UINT64_C(0xa5a55a5af00dcafe) || zero_fill[0] || zero_fill[4] ||
        native_tls_module_read() != UINT64_C(0x13579bdf2468ace0) || native_tls_module_zero())
        return 1;
    initialized = 77;
    zero_fill[4] = 88;
    native_tls_module_write(99);
    native_tls_zero_write(111);
    pthread_t threads[2];
    struct worker_state states[2] = {{.id = 1}, {.id = 2}};
    for (unsigned n = 0; n != 2; ++n)
        if (pthread_create(&threads[n], 0, worker, &states[n]))
            return 2;
    pthread_mutex_lock(&lock);
    while (arrived != 2)
        pthread_cond_wait(&condition, &lock);
    int separate = states[0].local_address != states[1].local_address &&
                   states[0].module_address != states[1].module_address &&
                   states[0].zero_address != states[1].zero_address;
    for (unsigned n = 0; n != 2; ++n)
        separate &= states[n].local_address != (uintptr_t)&initialized &&
                    states[n].module_address != (uintptr_t)native_tls_module_address() &&
                    states[n].zero_address != (uintptr_t)native_tls_zero_address();
    released = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    for (unsigned n = 0; n != 2; ++n) {
        void* result = 0;
        if (pthread_join(threads[n], &result) || result != &states[n] || states[n].failure)
            return 3;
    }
    if (!separate || initialized != 77 || zero_fill[4] != 88 ||
        native_tls_module_read() != 99 || native_tls_module_zero() != 100 || native_tls_zero_read() != 111)
        return 4;
    if (destructor_count != 4)
        return 5;
    unsigned found = 0;
    unsigned module_seen = 0;
    for (unsigned n = 0; n != destructor_count; ++n) {
        int value = destructor_values[n];
        if (value == 11) { if (!(module_seen & 1)) return 8; found |= 1; }
        else if (value == 12) { if (!(module_seen & 2)) return 8; found |= 2; }
        else if (value == 21) { found |= 4; module_seen |= 1; }
        else if (value == 22) { found |= 8; module_seen |= 2; }
        else return 6;
    }
    return found == 15 ? dynamic_check() : 7;
}
