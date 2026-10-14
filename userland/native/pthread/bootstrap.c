#include <pthread.h>

/* The implementation and main-thread storage are Apple's VARIANT_DYLD build. */
extern void _pthread_set_self(pthread_t);

void
native_pthread_bootstrap(void)
{
    _pthread_set_self(0);
}
