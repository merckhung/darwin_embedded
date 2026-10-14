/* Native LibSystem startup for the implemented Darwin component set.
 * Callback versions and ordering follow Libsystem-1356/init.c and the
 * matching component headers. Services absent from this runtime are not
 * initialized or advertised by substitute entry points.
 */
#include "_libkernel_init.h"
#include "_libc_init.h"
#include "native_programvars.h"
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <malloc/_malloc_type.h>
#include <pthread.h>
#include <pthread/private.h>
#include <stdlib.h>

extern void native_system_bootstrap(void);
extern void native_tlv_init(void);
#ifdef NATIVE_COMPILER_TLS
extern void native_tls_init(void);
#endif
extern void _pthread_exit_if_canceled(int);
extern void _pthread_clear_qos_tsd(mach_port_t);
extern int pthread_current_stack_contains_np(const void *, size_t);
extern int __pthread_init(const struct _libpthread_functions *, const char *[],
                          const char *[], const struct ProgramVars *);
extern void __malloc_init(const char *[]);
extern void __libplatform_init(void *, const char *[], const char *[], const struct ProgramVars *);
extern void _mach_fork_child(void);
extern void _malloc_fork_prepare(void);
extern void _malloc_fork_parent(void);
extern void _malloc_fork_child(void);
extern void _pthread_atfork_prepare(void);
extern void _pthread_atfork_parent(void);
extern void _pthread_atfork_child(void);
extern void _pthread_atfork_prepare_handlers(void);
extern void _pthread_atfork_parent_handlers(void);
extern void _pthread_atfork_child_handlers(void);
extern void _dyld_dlopen_atfork_prepare(void);
extern void _dyld_dlopen_atfork_parent(void);
extern void _dyld_dlopen_atfork_child(void);

static void prepare(unsigned flags, ...)
{
    _dyld_dlopen_atfork_prepare();
    if (!(flags & LIBSYSTEM_ATFORK_HANDLERS_ONLY_FLAG))
        _pthread_atfork_prepare_handlers();
    _malloc_fork_prepare();
    _libc_fork_prepare();
    _pthread_atfork_prepare();
}

static void parent(unsigned flags, ...)
{
    _pthread_atfork_parent();
    _malloc_fork_parent();
    _libc_fork_parent();
    _dyld_dlopen_atfork_parent();
    if (!(flags & LIBSYSTEM_ATFORK_HANDLERS_ONLY_FLAG))
        _pthread_atfork_parent_handlers();
}

static void child(unsigned flags, ...)
{
    _mach_fork_child();
    _pthread_atfork_child();
    _malloc_fork_child();
    _libc_fork_child();
    _dyld_dlopen_atfork_child();
    if (!(flags & LIBSYSTEM_ATFORK_HANDLERS_ONLY_FLAG))
        _pthread_atfork_child_handlers();
}

static const struct _libkernel_functions kernel_functions = {
    .version = 5,
    .dlsym = dlsym,
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
static const struct _libc_functions libc_functions = {
    .version = 2,
    .atfork_prepare_v2 = prepare,
    .atfork_parent_v2 = parent,
    .atfork_child_v2 = child,
};

__attribute__((constructor))
static void initialize(int argc, char **argv, char **envp, char **apple,
                       const struct ProgramVars *vars)
{
    (void)argc;
    (void)argv;
    native_system_bootstrap();
    __libkernel_init(&kernel_functions, (const char **)envp, (const char **)apple, vars);
    __libplatform_init(0, (const char **)envp, (const char **)apple, vars);
    if (__pthread_init(&pthread_functions, (const char **)envp, (const char **)apple, vars))
        abort();
    _libc_initializer(&libc_functions, (const char **)envp, (const char **)apple, vars);
    __malloc_init((const char **)apple);
    native_tlv_init();
#ifdef NATIVE_COMPILER_TLS
    native_tls_init();
#endif
}
