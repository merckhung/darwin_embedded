#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {

int32_t OSAtomicIncrement32(volatile int32_t *value) {
    return __sync_add_and_fetch(value, 1);
}

// libcodedirectory stubs
struct libcd;
typedef struct libcd libcd;
libcd* libcd_create(size_t s) { return NULL; }
int libcd_set_hash_types_for_platform_version(libcd* s, int p, int v) { return 0; }
void libcd_set_signing_id(libcd* s, const char* id) {}
void libcd_set_flags(libcd* s, uint32_t f) {}
size_t libcd_superblob_size(libcd* s) { return 0; }
void libcd_set_input_mem(libcd* s, const void* p, size_t len) {}
void libcd_set_output_mem(libcd* s, void* p, size_t len) {}
int libcd_serialize(libcd* s) { return 0; }

}
