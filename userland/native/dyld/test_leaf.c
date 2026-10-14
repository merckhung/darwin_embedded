#include <stdint.h>
static uintptr_t cookie = 19;
static uintptr_t *volatile cookie_pointer = &cookie;
__attribute__((constructor)) static void initialize_leaf(void) { *cookie_pointer = 31; }
int native_double(int value) { return value * 2 + (int)*cookie_pointer; }
