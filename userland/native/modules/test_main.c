#include <dlfcn.h>
#include <errno.h>
#include <mach-o/dyld_priv.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

static const char module_path[] = "/usr/lib/native-modules/libmodule.dylib";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned events[256], event_count, blocked, entered, errors_ready;
static unsigned fork_started, fork_finished, callback_seen, callback_errors;
static unsigned close_hook, close_entered, close_error, close_attempted;
static unsigned initializer_entered, initializer_release, initializer_child, initializer_error, opener_attempted;
static unsigned finalizer_child, finalizer_error, baseline_images;
static const char slow_path[] = "/usr/lib/native-modules/libslow.dylib";
static const char forking_path[] = "/usr/lib/native-modules/libforking.dylib";
static const struct mach_header *removal_header;
static void *removal_function;
static unsigned removal_seen,removal_errors;

void native_module_event(int event)
{
    pthread_mutex_lock(&lock);
    if (event_count < 256) events[event_count++] = (unsigned)event;
    if (blocked == (unsigned)event) {
        entered = 1;
        pthread_cond_broadcast(&changed);
        while (blocked) pthread_cond_wait(&changed, &lock);
    }
    pthread_mutex_unlock(&lock);
}

void native_module_close_hook(void)
{
    unsigned mode = __atomic_load_n(&close_hook, __ATOMIC_ACQUIRE);
    if (!mode) return;
    if (mode == 2) {
        pid_t child = fork();
        if (child < 0) { finalizer_error = 1; return; }
        if (!child) { finalizer_child = 1; return; }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
            finalizer_error = 2;
        return;
    }
    __atomic_store_n(&close_entered, 1, __ATOMIC_RELEASE);
    void *handle = dlopen(slow_path, RTLD_NOW | RTLD_LOCAL);
    int (*function)(void) = handle ? dlsym(handle, "native_slow") : 0;
    if (!function || function() != 71 || dlclose(handle))
        __atomic_store_n(&close_error, 1, __ATOMIC_RELEASE);
}

void native_module_fork_hook(void)
{
    pthread_mutex_lock(&lock);
    initializer_entered = 1;
    pthread_cond_broadcast(&changed);
    while (!initializer_release) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    pid_t child = fork();
    if (child < 0) { initializer_error = 1; return; }
    if (!child) { initializer_child = 1; return; }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        initializer_error = 2;
}

static unsigned image_count(void)
{
    unsigned count = 0;
    while (_dyld_get_image_header(count)) ++count;
    return count;
}

static int mapped(const void *pointer)
{
    mach_vm_address_t address = (uintptr_t)pointer;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = mach_vm_region(mach_task_self(), &address, &size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (result == KERN_INVALID_ADDRESS) return 0;
    if (result != KERN_SUCCESS) return -1;
    return address <= (uintptr_t)pointer && (uintptr_t)pointer - address < size;
}

static void removal(const struct mach_header *header,intptr_t slide)
{
    if (header!=removal_header)return;
    (void)slide;
    if (event_count!=4 || events[2]!=3 || events[3]!=4 ||
        mapped(header)!=1 || mapped(removal_function)!=1 ||
        dlsym(RTLD_DEFAULT,"native_module_event")!=native_module_event)
        ++removal_errors;
    ++removal_seen;
}

static void notification(unsigned count, const struct mach_header *headers[], const char *paths[])
{
    /* Call metadata and dlsym back through the real recursive API path. */
    for (unsigned n = 0; n < count; ++n) {
        if (!headers[n] || !paths[n] ||
            dyld_image_header_containing_address(headers[n]) != headers[n])
            __atomic_add_fetch(&callback_errors, 1, __ATOMIC_RELAXED);
        if (!strcmp(paths[n], module_path)) {
            if (dlsym(RTLD_DEFAULT, "native_module_event") != native_module_event)
                __atomic_add_fetch(&callback_errors, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&callback_seen, 1, __ATOMIC_RELAXED);
        }
    }
}

static int one_shot_error(const char *fragment)
{
    char *error = dlerror();
    return error && strstr(error, fragment) && !dlerror();
}

static void *error_worker(void *opaque)
{
    const char *name = opaque;
    if (dlsym(RTLD_DEFAULT, name)) return (void *)1;
    pthread_mutex_lock(&lock);
    ++errors_ready;
    pthread_cond_broadcast(&changed);
    while (errors_ready != 2) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    return one_shot_error(name) ? 0 : (void *)2;
}

static void *load_worker(void *opaque)
{
    unsigned rounds = (uintptr_t)opaque;
    for (unsigned n = 0; n < rounds; ++n) {
        void *handle = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) return (void *)3;
        int (*function)(int) = dlsym(handle, "native_module");
        if (!function || function(9) != 29 || dlerror()) return (void *)4;
        if (dlclose(handle)) return (void *)5;
    }
    return 0;
}

static void *fork_worker(void *unused)
{
    (void)unused;
    __atomic_store_n(&fork_started, 1, __ATOMIC_RELEASE);
    pid_t child = fork();
    if (child < 0) return (void *)6;
    if (!child) {
        void *handle = dlopen(module_path, RTLD_NOW | RTLD_NOLOAD);
        int (*function)(int) = handle ? dlsym(handle, "native_module") : 0;
        _exit(function && function(13) == 37 && !dlclose(handle) ? 0 : 7);
    }
    int status = 0;
    pid_t waited = waitpid(child, &status, 0);
    __atomic_store_n(&fork_finished, 1, __ATOMIC_RELEASE);
    return waited == child && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : (void *)8;
}

static void *held_module;
static void *held_slow;
static void *slow_load(void *unused)
{
    (void)unused;
    held_slow = dlopen(slow_path, RTLD_NOW | RTLD_LOCAL);
    return held_slow ? 0 : (void *)10;
}
static void *held_close(void *unused)
{
    (void)unused;
    __atomic_store_n(&close_attempted, 1, __ATOMIC_RELEASE);
    int result = dlclose(held_module);
    if (finalizer_child) {
        if (result || image_count() != baseline_images) _exit(15);
        void *bundle = dlopen("/usr/lib/native-modules/native.bundle", RTLD_NOW);
        int (*function)(int) = bundle ? dlsym(bundle, "native_bundle") : 0;
        _exit(function && function(5) == 42 && !dlclose(bundle) ? 0 : 16);
    }
    return result ? (void *)11 : 0;
}
static void *held_forking;
static void *forking_load(void *unused)
{
    (void)unused;
    held_forking = dlopen(forking_path, RTLD_NOW | RTLD_LOCAL);
    if (initializer_child) {
        int (*function)(void) = held_forking ? dlsym(held_forking, "native_forking") : 0;
        _exit(function && function() == 83 && !dlclose(held_forking) ? 0 : 12);
    }
    return held_forking ? 0 : (void *)13;
}
static void *forking_waiter(void *unused)
{
    (void)unused;
    __atomic_store_n(&opener_attempted, 1, __ATOMIC_RELEASE);
    void *handle = dlopen(forking_path, RTLD_NOW | RTLD_LOCAL);
    int (*function)(void) = handle ? dlsym(handle, "native_forking") : 0;
    return function && function() == 83 && !dlclose(handle) ? 0 : (void *)14;
}
static void *held_load(void *unused)
{
    (void)unused;
    held_module = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    return held_module ? 0 : (void *)9;
}

static int check_weak(void)
{
    unsigned initial=image_count();
    const char *a_path="/usr/lib/native-modules/libweak_a.dylib";
    const char *b_path="/usr/lib/native-modules/libweak_b.dylib";
    void *a=dlopen(a_path,RTLD_NOW|RTLD_GLOBAL);
    int *(*a_address)(void)=a?dlsym(a,"native_weak_a_address"):0;
    if (!a_address || *a_address()!=17)return 68;
    void *b=dlopen(b_path,RTLD_NOW|RTLD_GLOBAL);
    int *(*b_address)(void)=b?dlsym(b,"native_weak_b_address"):0;
    if (!b_address || b_address()!=a_address() || *b_address()!=17)return 69;
    void *strong=dlopen("/usr/lib/native-modules/libweak_strong.dylib",RTLD_NOW|RTLD_GLOBAL);
    int *(*strong_address)(void)=strong?dlsym(strong,"native_strong_address"):0;
    if (!strong_address || a_address()!=strong_address() || b_address()!=strong_address() || *a_address()!=43)return 70;
    if (dlclose(strong) || *b_address()!=43 || dlclose(a) || *b_address()!=43 || dlclose(b) || image_count()!=initial)return 71;
    a=dlopen(a_path,RTLD_NOW|RTLD_LOCAL);a_address=a?dlsym(a,"native_weak_a_address"):0;
    if (!a_address || *a_address()!=17)return 72;
    b=dlopen(b_path,RTLD_NOW|RTLD_GLOBAL);b_address=b?dlsym(b,"native_weak_b_address"):0;
    if (!b_address || *b_address()!=29 || a_address()!=b_address())return 73;
    if (dlclose(b) || *a_address()!=29 || dlclose(a) || image_count()!=initial)return 74;
    /* Reuse an earlier registry slot without changing real load order. */
    void *temporary=dlopen("/usr/lib/native-modules/native.bundle",RTLD_NOW|RTLD_LOCAL);
    b=dlopen(b_path,RTLD_NOW|RTLD_GLOBAL);b_address=b?dlsym(b,"native_weak_b_address"):0;
    if (!temporary || !b_address || dlclose(temporary))return 75;
    a=dlopen(a_path,RTLD_NOW|RTLD_GLOBAL);a_address=a?dlsym(a,"native_weak_a_address"):0;
    if (!a_address || a_address()!=b_address() || *a_address()!=29)return 76;
    if (dlclose(b) || *a_address()!=29 || dlclose(a) || image_count()!=initial)return 77;
    return 0;
}

static int check(void)
{
    unsigned initial = image_count();
    baseline_images = initial;
    _dyld_register_for_bulk_image_loads(notification);
    _dyld_register_func_for_remove_image(removal);
    struct dyld_unwind_sections unwind;
    if (!_dyld_find_unwind_sections((void *)check,&unwind) || !unwind.mh ||
        !unwind.compact_unwind_section || !unwind.compact_unwind_section_length) return 64;
    if (_dyld_find_unwind_sections((void *)check,NULL) ||
        _dyld_find_unwind_sections((void *)1,&unwind) || unwind.mh ||
        unwind.dwarf_section || unwind.dwarf_section_length ||
        unwind.compact_unwind_section || unwind.compact_unwind_section_length) return 65;
    if (dlopen("/does/not/exist.dylib", RTLD_NOW) || !one_shot_error("cannot open")) return 1;
    if (dlopen(module_path, RTLD_NOW | RTLD_NOLOAD) || !one_shot_error("not loaded")) return 2;
    if (dlopen(module_path, 0x40000000) || !one_shot_error("mode")) return 3;
    for (unsigned n = 0; n < 3; ++n) {
        if (dlopen("/usr/lib/native-modules/libbroken.dylib", RTLD_NOW) ||
            !one_shot_error("native_intentionally_missing_import")) return 4;
        if (image_count() != initial || event_count) return 5;
    }
    void *handle = dlopen("@rpath/libmodule.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!handle || image_count() != initial + 2 || event_count != 2 || events[0] != 1 || events[1] != 2) return 6;
    int (*function)(int) = dlsym(handle, "native_module");
    int (*helper)(int) = dlsym(handle, "native_helper");
    if (!function || !helper || function(8) != 27 || helper(8) != 16 || dlerror() || mapped((void *)function) != 1) return 7;
    if (!_dyld_find_unwind_sections((void *)function,&unwind) ||
        unwind.mh!=dyld_image_header_containing_address((void *)function) ||
        !unwind.compact_unwind_section || !unwind.compact_unwind_section_length ||
        mapped(unwind.compact_unwind_section)!=1) return 66;
    removal_header=unwind.mh;removal_function=(void *)function;
    if (dlsym(handle, "native_absolute_zero") || dlerror()) return 8;
    if (dlsym(RTLD_DEFAULT, "native_module") || !one_shot_error("native_module")) return 9;
    if (dlsym(RTLD_DEFAULT, "native_helper") != helper || dlerror()) return 10;
    void *first = dlopen(module_path, RTLD_NOW | RTLD_FIRST | RTLD_LOCAL);
    if (!first || dlsym(first, "native_helper") || !one_shot_error("native_helper")) return 11;
    void *again = dlopen(module_path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (again != handle || event_count != 2 || dlclose(first) || dlclose(again)) return 12;
    if (function(5) != 21 || dlclose(handle)) return 13;
    if (image_count() != initial || event_count != 4 || events[2] != 3 || events[3] != 4 ||
        dyld_image_header_containing_address((void *)function)) return 14;
    if (mapped((void *)function) != 0) return 15;
    if (removal_seen!=1 || removal_errors ||
        _dyld_find_unwind_sections((void *)function,&unwind) || unwind.mh) return 67;
    removal_header=NULL;removal_function=NULL;
    if (!dlclose(handle) || !one_shot_error("handle")) return 16;

    void *bundle = dlopen("@executable_path/../usr/lib/native-modules/native.bundle", RTLD_NOW);
    int (*bundle_function)(int) = bundle ? dlsym(bundle, "native_bundle") : 0;
    if (!bundle_function || bundle_function(5) != 42 || dlclose(bundle)) return 17;
    pthread_t threads[2]; void *result;
    if (pthread_create(threads, 0, error_worker, "absent_thread_alpha") ||
        pthread_create(threads + 1, 0, error_worker, "absent_thread_beta")) return 18;
    for (unsigned n = 0; n < 2; ++n)
        if (pthread_join(threads[n], &result) || result) return 19;
    if (dlerror()) return 20;
    for (unsigned n = 0; n < 2; ++n)
        if (pthread_create(threads + n, 0, load_worker, (void *)8)) return 21;
    for (unsigned n = 0; n < 2; ++n)
        if (pthread_join(threads[n], &result) || result) return 22;
    if (image_count() != initial) return 23;

    pthread_mutex_lock(&lock);
    blocked = 1;
    if (pthread_create(threads, 0, held_load, 0)) return 24;
    while (!entered) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    if (pthread_create(threads + 1, 0, fork_worker, 0)) return 25;
    while (!__atomic_load_n(&fork_started, __ATOMIC_ACQUIRE)) usleep(1000);
    usleep(20000);
    if (__atomic_load_n(&fork_finished, __ATOMIC_ACQUIRE)) return 26;
    pthread_mutex_lock(&lock);
    blocked = 0;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (unsigned n = 0; n < 2; ++n)
        if (pthread_join(threads[n], &result) || result) return 27;
    if (dlclose(held_module) || image_count() != initial) return 28;

    /* Another thread requests finalization while a target is initializing.
     * Mutation serialization delays the finalizer, which then reenters dlopen. */
    held_module = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (!held_module) return 32;
    pthread_mutex_lock(&lock);
    blocked = 5; entered = 0;
    if (pthread_create(threads, 0, slow_load, 0)) return 33;
    while (!entered) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    __atomic_store_n(&close_hook, 1, __ATOMIC_RELEASE);
    if (pthread_create(threads + 1, 0, held_close, 0)) return 34;
    while (!__atomic_load_n(&close_attempted, __ATOMIC_ACQUIRE)) usleep(1000);
    usleep(20000);
    if (__atomic_load_n(&close_entered, __ATOMIC_ACQUIRE)) return 37;
    pthread_mutex_lock(&lock);
    blocked = 0;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (unsigned n = 0; n < 2; ++n)
        if (pthread_join(threads[n], &result) || result) return 35;
    __atomic_store_n(&close_hook, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&close_error, __ATOMIC_ACQUIRE) || dlclose(held_slow) || image_count() != initial) return 36;

    /* The initializer owns the mutation lock and forks while another opener
     * is waiting on that lock. The child returns through the actual initializer
     * and dlopen frames, then looks up/calls/closes the module before exiting. */
    pthread_mutex_lock(&lock);
    if (pthread_create(threads, 0, forking_load, 0)) return 38;
    while (!initializer_entered) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    if (pthread_create(threads + 1, 0, forking_waiter, 0)) return 39;
    while (!__atomic_load_n(&opener_attempted, __ATOMIC_ACQUIRE)) usleep(1000);
    usleep(20000);
    pthread_mutex_lock(&lock);
    initializer_release = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (unsigned n = 0; n < 2; ++n)
        if (pthread_join(threads[n], &result) || result) return 40;
    if (initializer_error || dlclose(held_forking) || image_count() != initial) return 41;

    held_module = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (!held_module) return 42;
    __atomic_store_n(&close_hook, 2, __ATOMIC_RELEASE);
    if (pthread_create(threads, 0, held_close, 0) || pthread_join(threads[0], &result) || result) return 43;
    __atomic_store_n(&close_hook, 0, __ATOMIC_RELEASE);
    if (finalizer_error || image_count() != initial) return 44;

    unsigned before = event_count;
    handle = dlopen(module_path, RTLD_NOW | RTLD_NODELETE);
    if (!handle || event_count != before + 2 || dlclose(handle) || event_count != before + 2) return 29;
    handle = dlopen(module_path, RTLD_NOW | RTLD_NOLOAD);
    function = handle ? dlsym(handle, "native_module") : 0;
    if (!function || function(2) != 15 || dlclose(handle) || event_count != before + 2) return 30;
    if (!__atomic_load_n(&callback_seen, __ATOMIC_RELAXED) ||
        __atomic_load_n(&callback_errors, __ATOMIC_RELAXED)) return 31;
    unsigned current_images = image_count();
    void *notifier = dlopen("/usr/lib/native-modules/libnotifier.dylib", RTLD_NOW | RTLD_LOCAL);
    unsigned (*notifications)(void) = notifier ? dlsym(notifier, "native_notifier_count") : 0;
    if (!notifications || !notifications() || dlclose(notifier) ||
        mapped((void *)notifications) != 1 || image_count() != current_images + 1) return 45;
    unsigned before_notification = notifications();
    bundle = dlopen("/usr/lib/native-modules/native.bundle", RTLD_NOW);
    if (!bundle || notifications() != before_notification + 1 || dlclose(bundle)) return 46;
    return check_weak();
}

static void say(const char *text) { write(1, text, strlen(text)); }
int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    int code = check();
    if (code) {
        char digits[12]; unsigned count = 0;
        say("Native modules failure: ");
        do { digits[count++] = '0' + code % 10; code /= 10; } while (code);
        while (count) write(1, digits + --count, 1);
        say("\n"); return 1;
    }
    char release[128], machine[128];
    int mib[2] = {CTL_KERN, KERN_OSRELEASE}; size_t size = sizeof(release);
    if (sysctl(mib, 2, release, &size, 0, 0) || !size || size > sizeof(release) || release[size - 1]) return 1;
    mib[0] = CTL_HW; mib[1] = HW_MACHINE; size = sizeof(machine);
    if (sysctl(mib, 2, machine, &size, 0, 0) || !size || size > sizeof(machine) || machine[size - 1]) return 1;
    say("NATIVE_MODULES_OK:"); say(argv[1]); say(":"); say(release); say(":"); say(machine); say("\n");
    return 0; /* The loader must invoke the real Libc exit implementation. */
}
