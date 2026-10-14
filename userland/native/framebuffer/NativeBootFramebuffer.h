/* Native boot-scanout access ABI. This is not the IOFramebuffer family ABI. */
#ifndef NATIVE_BOOT_FRAMEBUFFER_H
#define NATIVE_BOOT_FRAMEBUFFER_H
#include <stdint.h>

enum {
    kNativeFramebufferConnection = 0,
    kNativeFramebufferInformation = 0,
    kNativeFramebufferPixelLayout = 1,
    kNativeFramebufferAcquireConsole = 2,
    kNativeFramebufferReleaseConsole = 3,
    kNativeFramebufferMemory = 0,
    kNativeFramebufferABIVersion = 1,
};

typedef struct NativeFramebufferInfo {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t row_bytes;
    uint32_t bits_per_pixel;
    uint32_t mapping_alignment; /* XNU PAGE_MAX_SIZE: safe for every supported task granule */
    uint64_t byte_length;
} NativeFramebufferInfo;

typedef struct NativeFramebufferPixelLayout {
    uint32_t version;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
} NativeFramebufferPixelLayout;

#endif
