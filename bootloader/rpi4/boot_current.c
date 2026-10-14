#include <stddef.h>
#include <stdint.h>
#include "adt_bin.h"
#include "kernel_layout.h"
#include "framebuffer.h"

#ifndef RPI_ROOTFS_SIZE
#define RPI_ROOTFS_SIZE 0x4000000
#endif
#define RPI_STRINGIFY_INNER(value) #value
#define RPI_STRINGIFY(value) RPI_STRINGIFY_INNER(value)
#include "../video/metadata.h"

/* XNU 12377.121.6 pexpert/arm64/boot.h, revision 2, version 2. */
struct Boot_Video {
    uint64_t base, display, row_bytes, width, height, depth;
};
struct boot_args {
    uint16_t revision, version;
    uint64_t virt_base, phys_base, mem_size, top_of_kernel_data;
    struct Boot_Video video;
    uint32_t machine_type;
    void *device_tree;
    uint32_t device_tree_length;
    char command_line[1024];
    uint64_t boot_flags, mem_size_actual;
};
_Static_assert(sizeof(struct boot_args) == 1152, "current boot argument size");
_Static_assert(offsetof(struct boot_args, command_line) == 108, "command line offset");
_Static_assert(offsetof(struct boot_args, boot_flags) == 1136, "boot flags offset");
_Static_assert(KERNEL_CPU_TYPE == 0x0100000c, "ARM64 kernel required");
_Static_assert(KERNEL_LOAD_ADDRESS + KERNEL_IMAGE_SIZE < 0x07f00000,
               "kernel overlaps the boot argument area");
_Static_assert(ADT_SEED_OFFSET + ADT_SEED_SIZE <= sizeof(adt_bin), "entropy bounds");
_Static_assert(ADT_FREQUENCY_OFFSET + sizeof(uint64_t) <= sizeof(adt_bin), "timer bounds");
_Static_assert(ADT_RAMDISK_OFFSET + 2 * sizeof(uint64_t) <= sizeof(adt_bin), "memory disk property bounds");
_Static_assert(0x0a000000ULL + RPI_ROOTFS_SIZE <= 0x20000000ULL, "memory disk exceeds managed RAM");
_Static_assert(0x07f18000 + sizeof(adt_bin) <= 0x07f20000,
               "device tree overlaps mailbox request");
_Static_assert(0x07f20000 + RPI_FB_WORDS * 4 <= 0x0a000000,
               "mailbox request overlaps the root memory disk");

void *memset(void *destination, int value, size_t size) {
    unsigned char *bytes = destination;
    while (size--) *bytes++ = (unsigned char)value;
    return destination;
}
void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *dst = destination;
    const unsigned char *src = source;
    while (size--) *dst++ = *src++;
    return destination;
}

static void uart_putc(char c) {
    volatile uint32_t *uart = (void *)0xfe201000;
    while (uart[0x18 / 4] & (1u << 5)) {}
    uart[0] = (unsigned char)c;
}
static void uart_puts(const char *text) {
    while (*text) {
        if (*text == '\n') uart_putc('\r');
        uart_putc(*text++);
    }
}
static void uart_hex(uint64_t value) {
    const char hex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        uart_putc(hex[(value >> shift) & 15]);
}

static uint64_t counter_ticks(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
    return value;
}

static int mailbox_framebuffer(uint64_t timer_frequency,
                               struct rpi_framebuffer *fb) {
    /* boot_entry.S disabled the MMU and D-cache. This low physical buffer is
     * reachable by VC DMA; the loader itself resides above the VC alias range.
     * No cache maintenance is needed, but DMA/MMIO ordering still requires DSB.
     */
    volatile uint32_t *message = (void *)0x07f20000;
    volatile uint32_t *mailbox = (void *)0xfe00b880;
    const uint32_t request = UINT32_C(0xc7f20008); /* uncached VC alias, channel 8 */
    if (!timer_frequency || timer_frequency > UINT64_MAX / 2)
        return 0;
    const uint64_t timeout = timer_frequency * 2;
    const uint64_t start = counter_ticks();
    rpi_framebuffer_request(message);
    __asm__ volatile("dsb sy" ::: "memory");
    while (mailbox[0x38 / 4] & (1u << 31)) {
        if (counter_ticks() - start >= timeout)
            return 0;
    }
    mailbox[0x20 / 4] = request;
    for (;;) {
        if (counter_ticks() - start >= timeout)
            return 0;
        if (mailbox[0x18 / 4] & (1u << 30))
            continue;
        if (mailbox[0] != request)
            continue;
        __asm__ volatile("dsb sy" ::: "memory");
        return rpi_framebuffer_validate(message, 2ULL << 30, 0x20000000, fb);
    }
}

void setup_and_boot(void *dtb) {
    (void)dtb;
    uart_puts("Raspberry Pi 4B loader: linked ARM64 Mach-O entry\n");
    struct boot_args *args = (void *)0x07f10000;
    void *tree = (void *)0x07f18000;
    memcpy(tree, adt_bin, sizeof(adt_bin));
    const uint64_t ramdisk_size = RPI_ROOTFS_SIZE;
    memcpy((unsigned char *)tree + ADT_RAMDISK_OFFSET + sizeof(uint64_t), &ramdisk_size, sizeof(ramdisk_size));
    /* QEMU's loader device supplies a fresh 256-byte host seed here. */
    volatile unsigned char *seed = (void *)0x07f00000;
    unsigned char seed_present = 0;
    for (size_t i = 0; i < ADT_SEED_SIZE; ++i) seed_present |= seed[i];
    if (!seed_present) {
        uart_puts("Missing per-boot entropy at 0x07f00000\n");
        for (;;) __asm__ volatile("wfe");
    }
    for (size_t i = 0; i < ADT_SEED_SIZE; ++i) {
        ((unsigned char *)tree)[ADT_SEED_OFFSET + i] = seed[i];
        seed[i] = 0;
    }
    uint64_t timer_frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_frequency));
    memcpy((unsigned char *)tree + ADT_FREQUENCY_OFFSET,
           &timer_frequency, sizeof(timer_frequency));
    memset(args, 0, sizeof(*args));
    args->revision = 2;
    args->version = 2;
    args->virt_base = KERNEL_VIRTUAL_BIAS;
    args->phys_base = 0;
    args->mem_size = args->mem_size_actual = 2ULL << 30;
    args->top_of_kernel_data = 0x20000000;
    struct rpi_framebuffer fb;
    if (!mailbox_framebuffer(timer_frequency, &fb)) {
        uart_puts("Framebuffer mailbox failed or returned invalid geometry/memory\n");
        for (;;) __asm__ volatile("wfe");
    }
    /* XNU ARM currently manages one contiguous physical range. Capping at the
     * firmware aperture reserves all VC memory, including its allocation metadata.
     * Physical DRAM remains 2 GiB in the ADT; only this lower range is available
     * to the VM. Loader code above 1 GiB is no longer needed after kernel entry.
     */
    args->mem_size = args->mem_size_actual = fb.managed_size;
    args->video.base = fb.base | 1; /* XNU physical-address marker */
    /* Start XNU's real text console; a later framebuffer driver can acquire
     * the display for Xorg. Graphics-boot mode would intentionally stay blank
     * with -noprogress and no userspace display server yet. */
    args->video.display = 0;
    args->video.row_bytes = fb.pitch;
    args->video.width = fb.width;
    args->video.height = fb.height;
    args->video.depth = 32;
    const struct native_boot_video video_metadata = {
        NATIVE_BOOT_VIDEO_VERSION, NATIVE_BOOT_VIDEO_BGRX,
        fb.base, fb.size, (uint32_t)fb.width, (uint32_t)fb.height,
        (uint32_t)fb.pitch, 32
    };
    memcpy((unsigned char *)tree + ADT_VIDEO_OFFSET, &video_metadata, sizeof(video_metadata));
    uart_puts("Framebuffer: base=0x"); uart_hex(fb.base);
    uart_puts(" width=0x"); uart_hex(fb.width);
    uart_puts(" height=0x"); uart_hex(fb.height);
    uart_puts(" pitch=0x"); uart_hex(fb.pitch);
    uart_puts("\nVC reserved: base=0x"); uart_hex(fb.vc_base);
    uart_puts(" size=0x"); uart_hex(fb.vc_size);
    uart_puts(" managed RAM=0x"); uart_hex(fb.managed_size);
    uart_puts("\n");
    args->device_tree = (void *)(KERNEL_VIRTUAL_BIAS + (uint64_t)tree);
    args->device_tree_length = sizeof(adt_bin);
    const char command[] =
        "-v -s -noprogress serial=3 serial-device-name=uart0 fastipi=0 debug=0x14c "
        "keepsyms=1 startup_debug=1 rd=md0 pmd0=0x0a000000." RPI_STRINGIFY(RPI_ROOTFS_SIZE);
    _Static_assert(sizeof(command) <= sizeof(args->command_line), "boot arguments too long");
    memcpy(args->command_line, command, sizeof(command));
    uart_puts("Kernel entry physical 0x");
    uart_hex(KERNEL_ENTRY_ADDRESS);
    uart_puts(", virtual bias 0x");
    uart_hex(KERNEL_VIRTUAL_BIAS);
    uart_puts("\n");
    __asm__ volatile("dsb sy\nisb" ::: "memory");
    void (*entry)(struct boot_args *) = (void *)KERNEL_ENTRY_ADDRESS;
    __asm__ volatile("mov x0, %0\nmov x21, #0\nblr %1"
                     :: "r"(args), "r"(entry) : "x0", "x21", "x30", "memory");
    for (;;) __asm__ volatile("wfe");
}
