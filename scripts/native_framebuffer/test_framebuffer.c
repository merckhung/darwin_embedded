#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bootloader/rpi4/framebuffer.h"

static void valid_reply(uint32_t *message)
{
    rpi_framebuffer_request(message);
    message[1] = RPI_FB_RESPONSE;
    for (size_t i = 2; i < RPI_FB_WORDS - 1; i += 3 + message[i + 1] / 4)
        message[i + 2] = RPI_FB_RESPONSE | message[i + 1];
    message[RPI_FB_ALLOCATE + 3] = 0xfc100000; /* genuine VC bus alias */
    message[RPI_FB_ALLOCATE + 4] = 1024 * 768 * 4;
    message[RPI_FB_PITCH + 3] = 1024 * 4;
    message[RPI_FB_VC_MEMORY + 3] = 0x3c000000;
    message[RPI_FB_VC_MEMORY + 4] = 0x04000000;
}

static void reject_change(size_t index, uint32_t value)
{
    uint32_t message[RPI_FB_WORDS];
    valid_reply(message);
    message[index] = value;
    struct rpi_framebuffer result;
    memset(&result, 0xa5, sizeof(result));
    struct rpi_framebuffer original = result;
    if (rpi_framebuffer_validate(message, UINT64_C(2) << 30, 0x20000000, &result)) {
        fprintf(stderr, "accepted malformed word %zu = 0x%x\n", index, value);
        assert(0);
    }
    assert(memcmp(&result, &original, sizeof(result)) == 0);
}

int main(void)
{
    uint32_t message[RPI_FB_WORDS];
    struct rpi_framebuffer result;
    valid_reply(message);
    assert(rpi_framebuffer_validate(message, UINT64_C(2) << 30, 0x20000000, &result));
    assert(result.base == 0x3c100000 && result.size == 3145728);
    assert(result.width == 1024 && result.height == 768 && result.pitch == 4096);
    assert(result.vc_base == 0x3c000000 && result.vc_size == 0x04000000);
    assert(result.managed_size == 0x3c000000);
    message[RPI_FB_ALLOCATE + 3] = 0x3c100000; /* QEMU returns physical address */
    assert(rpi_framebuffer_validate(message, UINT64_C(2) << 30, 0x20000000, &result));

    reject_change(0, sizeof(message) - 4);
    reject_change(1, 0);
    reject_change(1, RPI_FB_RESPONSE | 1);
    reject_change(RPI_FB_WORDS - 1, 1);
    for (size_t i = 2; i < RPI_FB_WORDS - 1; i += 3 + message[i + 1] / 4) {
        reject_change(i, message[i] ^ 1);
        reject_change(i + 1, message[i + 1] + 4);
        reject_change(i + 2, message[i + 1]); /* no response bit */
        reject_change(i + 2, RPI_FB_RESPONSE | (message[i + 1] - 4));
        reject_change(i + 2, RPI_FB_RESPONSE | (message[i + 1] + 4));
    }
    reject_change(RPI_FB_PHYSICAL + 3, 0);
    reject_change(RPI_FB_PHYSICAL + 3, UINT32_MAX);
    reject_change(RPI_FB_PHYSICAL + 4, 0);
    reject_change(RPI_FB_PHYSICAL + 4, 4097);
    reject_change(RPI_FB_VIRTUAL + 3, 2048);
    reject_change(RPI_FB_VIRTUAL + 4, 1536);
    reject_change(RPI_FB_DEPTH + 3, 24);
    reject_change(RPI_FB_ORDER + 3, 1);
    reject_change(RPI_FB_OFFSET + 3, 1);
    reject_change(RPI_FB_OFFSET + 4, 1);
    for (size_t i = 3; i < 7; ++i)
        reject_change(RPI_FB_OVERSCAN + i, 1);
    reject_change(RPI_FB_PITCH + 3, 0);
    reject_change(RPI_FB_PITCH + 3, 4092);
    reject_change(RPI_FB_PITCH + 3, 4097);
    reject_change(RPI_FB_PITCH + 3, UINT32_MAX - 3);
    reject_change(RPI_FB_ALLOCATE + 3, 0);
    reject_change(RPI_FB_ALLOCATE + 3, 0x3c100001);
    reject_change(RPI_FB_ALLOCATE + 3, 0x3bffc000);
    reject_change(RPI_FB_ALLOCATE + 3, 0x3fffffff);
    reject_change(RPI_FB_ALLOCATE + 4, 0);
    reject_change(RPI_FB_ALLOCATE + 4, 3145727);
    reject_change(RPI_FB_ALLOCATE + 4, UINT32_MAX);
    reject_change(RPI_FB_VC_MEMORY + 3, 0x20000000);
    reject_change(RPI_FB_VC_MEMORY + 3, 0x40000000);
    reject_change(RPI_FB_VC_MEMORY + 3, 0xc0000000);
    reject_change(RPI_FB_VC_MEMORY + 4, 0);
    reject_change(RPI_FB_VC_MEMORY + 4, UINT32_MAX);
    reject_change(RPI_FB_VC_MEMORY + 4, 0x00100000);

    valid_reply(message);
    assert(!rpi_framebuffer_validate(message, 0x3fffffff, 0x20000000, &result));
    assert(!rpi_framebuffer_validate(message, UINT64_C(2) << 30, UINT64_MAX, &result));
    assert(!rpi_framebuffer_validate(message, UINT64_C(2) << 30, 0x38000001, &result));
    assert(rpi_framebuffer_validate(message, UINT64_C(2) << 30, 0x38000000, &result));
    /* Firmware aperture start may be unaligned: reserve its partial page too. */
    message[RPI_FB_VC_MEMORY + 3] += 1;
    message[RPI_FB_VC_MEMORY + 4] -= 1;
    assert(rpi_framebuffer_validate(message, UINT64_C(2) << 30, 0x20000000, &result));
    assert(result.managed_size == 0x3c000000);
    puts("Framebuffer geometry, property replies and VC memory reservation verified");
    return 0;
}
