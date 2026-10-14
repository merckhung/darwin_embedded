/* Link as a genuine MH_DYLIB; these are compiler-emitted Mach-O TLVs. */
#include <stdint.h>
#include <mach-o/dyld_priv.h>

static _Thread_local uint64_t initialized = UINT64_C(0x13579bdf2468ace0);
static __thread uint64_t zero_fill[3];
static _Thread_local int lifetime;
static int constructor_ready;

__attribute__((constructor)) static void initialize_module(void)
{
    constructor_ready = initialized == UINT64_C(0x13579bdf2468ace0) &&
                        !(zero_fill[0] | zero_fill[1] | zero_fill[2]);
}

uint64_t native_tls_module_read(void) { return initialized; }
uint64_t native_tls_module_zero(void) { return zero_fill[0] | zero_fill[1] | zero_fill[2]; }
void native_tls_module_write(uint64_t value) { initialized = value; zero_fill[1] = value + 1; }
void *native_tls_module_address(void) { return &initialized; }
int native_tls_module_ready(void) { return constructor_ready; }
void native_tls_module_destructor(void (*function)(void*), int value)
{
    lifetime = value;
    _tlv_atexit(function, &lifetime);
}
