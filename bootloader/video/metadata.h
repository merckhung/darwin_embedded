/* Additive /chosen property for this source-built boot scanout driver.
 * Native little-endian words on both supported CPUs; no boot_args ABI change.
 * The bootloader fills every field only after validating real firmware data. */
#ifndef NATIVE_BOOT_VIDEO_METADATA_H
#define NATIVE_BOOT_VIDEO_METADATA_H
#include <stdint.h>

#define NATIVE_BOOT_VIDEO_PROPERTY "native-framebuffer"
#define NATIVE_BOOT_VIDEO_VERSION 1u
#define NATIVE_BOOT_VIDEO_RGBX 1u /* byte order: R G B reserved */
#define NATIVE_BOOT_VIDEO_BGRX 2u /* byte order: B G R reserved */

struct native_boot_video {
    uint32_t version, format;
    uint64_t physical_base, aperture_length;
    uint32_t width, height, row_bytes, bits_per_pixel;
};

static inline int native_boot_video_matches(const struct native_boot_video *video,
    uint64_t base, uint64_t length, uint32_t width, uint32_t height, uint32_t row_bytes)
{
    return video->version == NATIVE_BOOT_VIDEO_VERSION &&
        (video->format == NATIVE_BOOT_VIDEO_RGBX || video->format == NATIVE_BOOT_VIDEO_BGRX) &&
        video->physical_base == base && video->aperture_length >= length &&
        video->aperture_length <= UINT64_MAX - base &&
        video->width == width && video->height == height &&
        video->row_bytes == row_bytes && video->bits_per_pixel == 32;
}

#if defined(__cplusplus)
static_assert(sizeof(struct native_boot_video) == 40, "boot video metadata layout");
#else
_Static_assert(sizeof(struct native_boot_video) == 40, "boot video metadata layout");
#endif
#endif
