#ifndef RPI4_FRAMEBUFFER_H
#define RPI4_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>

/* Raspberry Pi firmware property channel, not a guest operating-system ABI.
 * https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 * Keep request construction and response validation independent of MMIO so
 * malformed firmware replies can be tested on the build host. */
enum {
    RPI_FB_WORDS = 47,
    RPI_FB_RESPONSE = 0x80000000u,
    RPI_FB_PHYSICAL = 2,
    RPI_FB_VIRTUAL = 7,
    RPI_FB_DEPTH = 12,
    RPI_FB_ORDER = 16,
    RPI_FB_OFFSET = 20,
    RPI_FB_OVERSCAN = 25,
    RPI_FB_ALLOCATE = 32,
    RPI_FB_PITCH = 37,
    RPI_FB_VC_MEMORY = 41,
    RPI_FB_PAGE_SIZE = 16384,
};

struct rpi_framebuffer {
    uint64_t base, size, width, height, pitch;
    uint64_t vc_base, vc_size, managed_size;
};

static void rpi_framebuffer_request(volatile uint32_t *message)
{
    static const uint32_t request[RPI_FB_WORDS] = {
        RPI_FB_WORDS * 4, 0,
        0x00048003, 8, 0, 1024, 768, /* physical dimensions */
        0x00048004, 8, 0, 1024, 768, /* virtual dimensions */
        0x00048005, 4, 0, 32,        /* 32-bit pixels */
        0x00048006, 4, 0, 0,         /* BGR, matching XNU PE_Video */
        0x00048009, 8, 0, 0, 0,      /* virtual offset */
        0x0004800a, 16, 0, 0, 0, 0, 0, /* no overscan */
        0x00040001, 8, 0, RPI_FB_PAGE_SIZE, 0, /* allocate */
        0x00040008, 4, 0, 0,         /* actual pitch */
        0x00010006, 8, 0, 0, 0,      /* entire VC memory reservation */
        0,
    };
    for (size_t i = 0; i < RPI_FB_WORDS; ++i)
        message[i] = request[i];
}

static int rpi_framebuffer_tag(const volatile uint32_t *message,
                               size_t offset, uint32_t tag, uint32_t size)
{
    return message[offset] == tag && message[offset + 1] == size &&
        message[offset + 2] == (RPI_FB_RESPONSE | size);
}

/* On failure, never publish even a partially validated video address. */
static int rpi_framebuffer_validate(const volatile uint32_t *message,
                                    uint64_t installed_size,
                                    uint64_t reserved_end,
                                    struct rpi_framebuffer *result)
{
    if (message[0] != RPI_FB_WORDS * 4 || message[1] != RPI_FB_RESPONSE ||
        message[RPI_FB_WORDS - 1] != 0 ||
        !rpi_framebuffer_tag(message, RPI_FB_PHYSICAL, 0x00048003, 8) ||
        !rpi_framebuffer_tag(message, RPI_FB_VIRTUAL, 0x00048004, 8) ||
        !rpi_framebuffer_tag(message, RPI_FB_DEPTH, 0x00048005, 4) ||
        !rpi_framebuffer_tag(message, RPI_FB_ORDER, 0x00048006, 4) ||
        !rpi_framebuffer_tag(message, RPI_FB_OFFSET, 0x00048009, 8) ||
        !rpi_framebuffer_tag(message, RPI_FB_OVERSCAN, 0x0004800a, 16) ||
        !rpi_framebuffer_tag(message, RPI_FB_ALLOCATE, 0x00040001, 8) ||
        !rpi_framebuffer_tag(message, RPI_FB_PITCH, 0x00040008, 4) ||
        !rpi_framebuffer_tag(message, RPI_FB_VC_MEMORY, 0x00010006, 8))
        return 0;

    struct rpi_framebuffer fb = {
        .base = message[RPI_FB_ALLOCATE + 3] & UINT32_C(0x3fffffff),
        .size = message[RPI_FB_ALLOCATE + 4],
        .width = message[RPI_FB_PHYSICAL + 3],
        .height = message[RPI_FB_PHYSICAL + 4],
        .pitch = message[RPI_FB_PITCH + 3],
        .vc_base = message[RPI_FB_VC_MEMORY + 3],
        .vc_size = message[RPI_FB_VC_MEMORY + 4],
    };
    if (fb.width < 64 || fb.width > 4096 || fb.height < 64 || fb.height > 4096 ||
        message[RPI_FB_VIRTUAL + 3] != fb.width ||
        message[RPI_FB_VIRTUAL + 4] != fb.height ||
        message[RPI_FB_DEPTH + 3] != 32 || message[RPI_FB_ORDER + 3] != 0 ||
        message[RPI_FB_OFFSET + 3] != 0 || message[RPI_FB_OFFSET + 4] != 0 ||
        message[RPI_FB_OVERSCAN + 3] != 0 || message[RPI_FB_OVERSCAN + 4] != 0 ||
        message[RPI_FB_OVERSCAN + 5] != 0 || message[RPI_FB_OVERSCAN + 6] != 0 ||
        fb.pitch < fb.width * 4 || (fb.pitch & 3) != 0 ||
        fb.pitch * fb.height > fb.size)
        return 0;

    /* VC bus aliases address only the first GiB. The whole firmware aperture
     * must be excluded from XNU's contiguous managed RAM, not just the pixels.
     * Subtraction keeps address/length bounds independent of integer wrap. */
    const uint64_t vc_limit = UINT64_C(0x40000000);
    if (!fb.vc_size || fb.vc_base >= vc_limit ||
        fb.vc_size > vc_limit - fb.vc_base ||
        fb.vc_base >= installed_size || fb.vc_size > installed_size - fb.vc_base ||
        fb.base < fb.vc_base || fb.base - fb.vc_base >= fb.vc_size ||
        !fb.size || fb.size > fb.vc_size - (fb.base - fb.vc_base) ||
        (fb.base & (RPI_FB_PAGE_SIZE - 1)) != 0)
        return 0;
    fb.managed_size = fb.vc_base & ~(uint64_t)(RPI_FB_PAGE_SIZE - 1);
    /* Leave usable memory beyond the kernel/ramdisk reservation. */
    if (reserved_end > fb.managed_size ||
        fb.managed_size - reserved_end < (UINT64_C(64) << 20))
        return 0;

    *result = fb;
    return 1;
}

#endif
