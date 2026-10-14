/* A separate all-zero TLS image exercises Apple's calloc instantiation path. */
#include <stdint.h>

static _Thread_local uint64_t value;
static int constructor_ready;

__attribute__((constructor)) static void initialize(void)
{
    constructor_ready = value == 0;
}

uint64_t native_tls_zero_read(void) { return value; }
void native_tls_zero_write(uint64_t next) { value = next; }
void *native_tls_zero_address(void) { return &value; }
int native_tls_zero_ready(void) { return constructor_ready; }
