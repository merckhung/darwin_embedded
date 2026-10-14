/* Run after native libSystem initialization, including __malloc_init. */
#include <errno.h>
#include <malloc/malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int native_malloc_check(void)
{
    static const size_t sizes[] = {1, 15, 16, 17, 255, 256, 257,
                                  4095, 4096, 16385, 1048577};
    unsigned char *allocations[sizeof(sizes) / sizeof(sizes[0])] = {0};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        allocations[i] = malloc(sizes[i]);
        if (!allocations[i]) return 1;
        if ((uintptr_t)allocations[i] % 16) return 2;
        if (malloc_size(allocations[i]) < sizes[i] ||
            !malloc_zone_from_ptr(allocations[i])) return 3;
        memset(allocations[i], (unsigned char)(0x31 + i), sizes[i]);
    }
    /* Keep every allocation live while checking independent contents. */
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        for (size_t j = 0; j < sizes[i]; ++j)
            if (allocations[i][j] != (unsigned char)(0x31 + i)) return 4;
        unsigned char *grown = realloc(allocations[i], sizes[i] + 4097);
        if (!grown) return 5;
        for (size_t j = 0; j < sizes[i]; ++j)
            if (grown[j] != (unsigned char)(0x31 + i)) return 6;
        allocations[i] = realloc(grown, sizes[i]);
        if (!allocations[i]) return 7;
        for (size_t j = 0; j < sizes[i]; ++j)
            if (allocations[i][j] != (unsigned char)(0x31 + i)) return 8;
    }
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        free(allocations[i]);

    unsigned char *zeroed = calloc(257, 17);
    if (!zeroed) return 9;
    for (size_t j = 0; j < 257 * 17; ++j)
        if (zeroed[j]) return 10;
    free(zeroed);
    volatile size_t overflowing_count = SIZE_MAX / 2 + 1;
    errno = 0;
    if (calloc(overflowing_count, 2) || errno != ENOMEM) return 11;

    for (size_t alignment = 16; alignment <= 65536; alignment *= 4) {
        void *aligned = NULL;
        if (posix_memalign(&aligned, alignment, alignment + 3) || !aligned)
            return 12;
        if ((uintptr_t)aligned % alignment) return 13;
        memset(aligned, 0xa7, alignment + 3);
        free(aligned);
    }
    free(NULL);
    if (!malloc_zone_check(NULL)) return 14;
    return 0;
}
