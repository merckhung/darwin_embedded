#include "_libkernel_init.h"
#include "_libc_init.h"
#include "native_programvars.h"
#include <malloc/malloc.h>
#include <malloc/_malloc_type.h>
#include <pthread.h>
#include <pthread/private.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <mach-o/loader.h>

extern void native_malloc_bootstrap(void);
extern int native_malloc_check(void);
extern int native_crypto_check(void);
extern int native_libc_check(void);
extern int native_rng_check(void);
extern int native_metadata_check(void);
extern void native_tlv_init(void);
extern int native_tlv_check(void);
extern void _pthread_exit_if_canceled(int);
extern void _pthread_clear_qos_tsd(mach_port_t);
extern int pthread_current_stack_contains_np(const void *, size_t);
extern int __pthread_init(const struct _libpthread_functions *, const char *[],
                          const char *[], const struct ProgramVars *);
extern void __malloc_init(const char *[]);
extern void __libplatform_init(void *, const char *[], const char *[], const struct ProgramVars *);
extern long __write_nocancel(int, const void *, size_t);
extern int __sysctl(const int *, unsigned, void *, size_t *, const void *, size_t);
extern const struct mach_header _mh_execute_header;

static const struct _libkernel_functions kernel_functions = {
    .version = 5,
    .malloc = malloc,
    .free = free,
    .realloc = realloc,
    ._pthread_exit_if_canceled = _pthread_exit_if_canceled,
    .pthread_clear_qos_tsd = _pthread_clear_qos_tsd,
    .pthread_current_stack_contains_np = pthread_current_stack_contains_np,
    .malloc_type_malloc = malloc_type_malloc,
    .malloc_type_free = malloc_type_free,
    .malloc_type_realloc = malloc_type_realloc,
};
static const struct _libpthread_functions pthread_functions = {
    .version = 2, .exit = exit, .malloc = malloc, .free = free,
};
/* These tests do not fork or request the optional CoreServices dirhelper. */
static const struct _libc_functions libc_functions = {.version = 2};

static void say(const char *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    __write_nocancel(1, text, length);
}

static int failure(unsigned code)
{
    char digits[12];
    unsigned n = 0;
    say("Native runtime failure: malloc:");
    do { digits[n++] = '0' + code % 10; code /= 10; } while (code);
    while (n) __write_nocancel(1, digits + --n, 1);
    say("\n");
    return 1;
}

int main(int argc, char **argv, char **envp, char **apple)
{
    if (argc != 2 || !apple) return failure(20);
    char *program_name = argv[0];
    struct ProgramVars vars = {
        .mh = (void *)&_mh_execute_header, .NXArgcPtr = &argc,
        .NXArgvPtr = &argv, .environPtr = &envp, .__prognamePtr = &program_name,
    };
    native_malloc_bootstrap();
    say("NATIVE_MALLOC_STAGE:bootstrap\n");
    __libkernel_init(&kernel_functions, (const char **)envp, (const char **)apple, &vars);
    say("NATIVE_MALLOC_STAGE:kernel\n");
    __libplatform_init(0, (const char **)envp, (const char **)apple, &vars);
    say("NATIVE_MALLOC_STAGE:platform\n");
    if (__pthread_init(&pthread_functions, (const char **)envp, (const char **)apple, &vars))
        return failure(21);
    say("NATIVE_MALLOC_STAGE:pthread\n");
    _libc_initializer(&libc_functions, (const char **)envp, (const char **)apple, &vars);
    say("NATIVE_MALLOC_STAGE:libc\n");
    __malloc_init((const char **)apple);
    say("NATIVE_MALLOC_STAGE:malloc\n");
    native_tlv_init();
    int code = native_malloc_check();
    if (code) return failure(code);
    code = native_libc_check();
    if (code) return failure(code);
    code = native_metadata_check();
    if (code) return failure(200 + code);
    code = native_crypto_check();
    if (code) return failure(code);
    code = native_rng_check();
    if (code) return failure(code);
    code = native_tlv_check();
    if (code) return failure(50 + code);
    char release[128] = {0}, machine[128] = {0};
    size_t release_size = sizeof(release), machine_size = sizeof(machine);
    int release_mib[] = {CTL_KERN, KERN_OSRELEASE};
    int machine_mib[] = {CTL_HW, HW_MACHINE};
    if (__sysctl(release_mib, 2, release, &release_size, 0, 0) ||
        __sysctl(machine_mib, 2, machine, &machine_size, 0, 0))
        return failure(22);
    say("NATIVE_MALLOC_OK:");
    say(argv[1]); say(":"); say(release); say(":"); say(machine); say("\n");
    exit(0);
}
