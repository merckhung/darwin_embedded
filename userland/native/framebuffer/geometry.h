/* Shared by the actual kernel driver and host rejection tests. */
#ifndef NATIVE_FRAMEBUFFER_GEOMETRY_H
#define NATIVE_FRAMEBUFFER_GEOMETRY_H
#include <stdint.h>

static inline uint64_t native_framebuffer_length(uint64_t base, uint64_t width,
    uint64_t height, uint64_t row_bytes, uint64_t depth, uint64_t offset,
    uint64_t explicit_length, uint64_t rotation, uint64_t alignment)
{
    if (!base || !width || !height || depth != 32 || offset || rotation ||
        width > UINT32_MAX / 4 || height > UINT32_MAX || row_bytes > UINT32_MAX ||
        row_bytes < width * 4 || (row_bytes & 3) ||
        !alignment || (alignment & (alignment - 1))) return 0;
    uint64_t length = row_bytes * height;
    if (!length || length > UINT64_C(64) * 1024 * 1024 || base > UINT64_MAX - length ||
        (base & (alignment - 1)) || (length & (alignment - 1)) ||
        (explicit_length && explicit_length < length)) return 0;
    return length;
}
#endif
