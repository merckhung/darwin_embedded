#include <pthread.h>

/* Apple's dyld pthread variant owns the actual initial thread and native TSD. */
extern void _pthread_set_self(pthread_t);

void native_malloc_bootstrap(void)
{
    _pthread_set_self(0);
}
