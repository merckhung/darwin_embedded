/* Hardware configuration for this repository's native Darwin QEMU targets. */
#ifndef NATIVE_PTHREAD_TARGET_CONFIG_H
#define NATIVE_PTHREAD_TARGET_CONFIG_H

#include <stdbool.h>

#if defined(NATIVE_CPU_CORTEX_A72) && defined(__arm64__)
/* Cortex-A72 has no Apple APRR per-thread JIT permission registers. */
#elif defined(NATIVE_CPU_PENRYN) && defined(__x86_64__)
/* Penryn has no per-thread MAP_JIT write-protection facility. */
#else
#error "Audit native JIT protection before adding a pthread CPU target"
#endif

/*
 * XNU's public os/thread_self_restrict.h omits these private declarations.
 * Apple documents unsupported platforms in pthread_jit_write_protect_np.3:
 * supported_np returns zero, protect_np returns, and callbacks run directly.
 * The unmodified pthread implementation selects those paths with this query.
 * A mistakenly reached hardware transition must fail, never report protection.
 */
static inline bool
os_thread_self_restrict_rwx_is_supported(void)
{
    return false;
}

static inline void
os_thread_self_restrict_rwx_to_rx(void)
{
    __builtin_trap();
}

static inline void
os_thread_self_restrict_rwx_to_rw(void)
{
    __builtin_trap();
}

/* Defined by the genuine XNU libsyscall/wrappers/_libkernel_init.c. */
extern bool _os_xbs_chrooted;

#endif
