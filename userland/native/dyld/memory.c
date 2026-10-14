/* Freestanding memory primitives used before the Darwin C runtime starts. */
#include "native_calls.h"
void *memcpy(void *destination, const void *source, size_t n) {
    volatile unsigned char *d = destination; const unsigned char *s = source;
    while (n--) *d++ = *s++;
    return destination;
}
void *memset(void *destination, int value, size_t n) {
    volatile unsigned char *d = destination;
    while (n--) *d++ = value;
    return destination;
}
void __bzero(void *destination, size_t n) { memset(destination, 0, n); }
void bzero(void *destination, size_t n) { memset(destination, 0, n); }
