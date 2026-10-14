#include "_libkernel_init.h"
#include <pthread.h>
#include <pthread/private.h>
#include <sys/sysctl.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

extern void native_pthread_bootstrap(void);
extern int native_pthread_probe(void);
extern void _pthread_exit_if_canceled(int);
extern void _pthread_clear_qos_tsd(mach_port_t);
extern int pthread_current_stack_contains_np(const void *, size_t);
extern int __pthread_init(const struct _libpthread_functions *, const char *[],
                          const char *[], const struct ProgramVars *);
extern long __write_nocancel(int, const void *, size_t);
extern int __sysctl(const int *, unsigned, void *, size_t *, const void *, size_t);

static const struct _libkernel_functions kernel_functions = {
    .version = 4,
    ._pthread_exit_if_canceled = _pthread_exit_if_canceled,
    .pthread_clear_qos_tsd = _pthread_clear_qos_tsd,
    .pthread_current_stack_contains_np = pthread_current_stack_contains_np,
};

static unsigned registered_images;
static int invalid_image;

static void
inspect_images(unsigned count, const struct mach_header *headers[], const char *paths[])
{
    registered_images += count;
    for (unsigned n = 0; n < count; ++n) {
        unsigned long size = 0;
        if (!headers[n] || headers[n]->magic != MH_MAGIC_64 || !paths[n] || !paths[n][0] ||
            !getsectiondata((const struct mach_header_64 *)headers[n], "__TEXT", "__text", &size) || !size)
            invalid_image = 1;
    }
}

static void
say(const char *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    __write_nocancel(1, text, length);
}

static int
failure(unsigned code)
{
    char digits[12];
    unsigned n = 0;
    say("Native runtime failure: pthread:");
    do { digits[n++] = '0' + code % 10; code /= 10; } while (code);
    while (n) __write_nocancel(1, digits + --n, 1);
    say("\n");
    return 1;
}

int
main(int argc, char **argv, char **envp, char **apple)
{
    if (argc != 2 || !apple) return failure(20);
    _dyld_register_for_bulk_image_loads(inspect_images);
    if (registered_images < 4 || invalid_image) return failure(23);
    native_pthread_bootstrap();
    /* Real callbacks; optional allocation upcalls remain unavailable in this
     * thread-only integration probe. __pthread_init retains its native __exit.
     */
    __libkernel_init(&kernel_functions, (const char **)envp, (const char **)apple, 0);
    if (__pthread_init(0, (const char **)envp, (const char **)apple, 0))
        return failure(21);
    int code = native_pthread_probe();
    if (code) return failure(code);
    char release[128] = {0}, machine[128] = {0};
    size_t release_size = sizeof(release), machine_size = sizeof(machine);
    int release_mib[] = {CTL_KERN, KERN_OSRELEASE};
    int machine_mib[] = {CTL_HW, HW_MACHINE};
    if (__sysctl(release_mib, 2, release, &release_size, 0, 0) ||
        __sysctl(machine_mib, 2, machine, &machine_size, 0, 0))
        return failure(22);
    say("NATIVE_PTHREAD_OK:");
    say(argv[1]); say(":"); say(release); say(":"); say(machine); say("\n");
    return 0;
}
