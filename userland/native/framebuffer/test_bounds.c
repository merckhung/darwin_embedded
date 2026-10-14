#include "geometry.h"
#include "bootloader/video/metadata.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint64_t base = 0x3c100000, size = 1024 * 768 * 4;
    uint64_t args[] = {base, 1024, 768, 4096, 32, 0, 0, 0, 16384};
#define LENGTH() native_framebuffer_length(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8])
    assert(LENGTH() == size);
    const struct { unsigned index; uint64_t value; } rejected[] = {
        {0, 0}, {0, base + 4096}, {0, UINT64_MAX - 16383},
        {1, 0}, {1, UINT64_MAX}, {1, UINT32_MAX},
        {2, 0}, {2, UINT64_MAX}, {2, UINT32_MAX}, {2, 769},
        {3, 4095}, {3, 4097}, {3, UINT64_MAX}, {3, UINT32_MAX - 3},
        {4, 24}, {5, 1}, {6, size - 1}, {7, 1}, {8, 0}, {8, 12288}
    };
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        uint64_t saved = args[rejected[i].index];
        args[rejected[i].index] = rejected[i].value;
        assert(LENGTH() == 0);
        args[rejected[i].index] = saved;
    }
    args[6] = size + 16384;
    assert(LENGTH() == size); /* map only pixels, not the extra firmware aperture */
    args[8] = 4096;
    assert(LENGTH() == size);

    struct native_boot_video metadata = {1, NATIVE_BOOT_VIDEO_BGRX, base, size, 1024, 768, 4096, 32};
#define MATCHES() native_boot_video_matches(&metadata, base, size, 1024, 768, 4096)
    assert(MATCHES());
    metadata.format = NATIVE_BOOT_VIDEO_RGBX;
    assert(MATCHES());
    metadata.format = 0; assert(!MATCHES());
    metadata.format = NATIVE_BOOT_VIDEO_BGRX;
    metadata.version = 2; assert(!MATCHES()); metadata.version = 1;
    metadata.physical_base++; assert(!MATCHES()); metadata.physical_base--;
    metadata.aperture_length = size - 1; assert(!MATCHES());
    metadata.aperture_length = UINT64_MAX; assert(!MATCHES());
    metadata.aperture_length = size;
    metadata.width++; assert(!MATCHES()); metadata.width--;
    metadata.height++; assert(!MATCHES()); metadata.height--;
    metadata.row_bytes++; assert(!MATCHES()); metadata.row_bytes--;
    metadata.bits_per_pixel = 24; assert(!MATCHES());
    puts("Native framebuffer: real driver geometry and boot metadata rejection tests passed");
    return 0;
}
