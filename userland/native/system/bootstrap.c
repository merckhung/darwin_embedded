/* The real VARIANT_DYLD pthread implementation establishes the initial TSD. */
#include <pthread.h>
extern void _pthread_set_self(pthread_t);

void native_system_bootstrap(void)
{
    _pthread_set_self(0);
}
