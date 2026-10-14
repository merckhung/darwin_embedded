/* Native scanout-memory validation. The visible stripes are a hardware probe,
 * not an X server or a desktop. Color masks come from actual firmware data. */
#include "NativeBootFramebuffer.h"
#include <NativeIOKit.h>
#include <NativeIOKitMIG.h>
#include <IOKit/IOMapTypes.h>
#include <mach/mach_host.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <grp.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#define CHECK(condition, label) do { if (!(condition)) { \
    fprintf(stderr, "NATIVE_FRAMEBUFFER_FAIL:%s\n", label); return 1; } } while (0)

static uint32_t test_pixel(uint32_t x, uint32_t width, const NativeFramebufferPixelLayout *layout)
{
    static const unsigned char colors[7][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
        {0, 255, 255}, {255, 0, 255}, {255, 255, 255}
    };
    const unsigned char *rgb = colors[x * 7 / width];
    return ((uint32_t)rgb[0] << __builtin_ctz(layout->red_mask)) |
           ((uint32_t)rgb[1] << __builtin_ctz(layout->green_mask)) |
           ((uint32_t)rgb[2] << __builtin_ctz(layout->blue_mask)) | layout->reserved_mask;
}

static int find_service(io_service_t *result)
{
    mach_port_t host = mach_host_self(), main_port = MACH_PORT_NULL;
    CHECK(host_get_io_main(host, &main_port) == KERN_SUCCESS && main_port, "main-port");
    CHECK(mach_port_deallocate(mach_task_self(), host) == KERN_SUCCESS, "release-host");
    io_iterator_t iterator = MACH_PORT_NULL;
    io_string_t match = "<dict><key>IOProviderClass</key><string>NativeBootFramebuffer</string></dict>";
    kern_return_t matching_result = KERN_FAILURE;
    // Current XNU intentionally disables the legacy fixed-string RPC. Its
    // real out-of-line method still accepts bounded serialized XML directly.
    CHECK(io_service_get_matching_services_ool(main_port, match, (mach_msg_type_number_t)strlen(match) + 1,
          &matching_result, &iterator) == KERN_SUCCESS && matching_result == KERN_SUCCESS, "match-service");
    CHECK(mach_port_deallocate(mach_task_self(), main_port) == KERN_SUCCESS, "release-main-port");
    io_service_t service = IOIteratorNext(iterator);
    CHECK(service && IOIteratorNext(iterator) == MACH_PORT_NULL, "unique-service");
    CHECK(IOObjectRelease(iterator) == KERN_SUCCESS, "release-iterator");
    *result = service;
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && strlen(argv[1]) == 48, "nonce");
    struct utsname identity;
    CHECK(uname(&identity) == 0, "uname");
    io_service_t service;
    CHECK(find_service(&service) == 0, "discover-service");
    io_connect_t connection = MACH_PORT_NULL, other = MACH_PORT_NULL;
    CHECK(IOServiceOpen(service, mach_task_self(), 9, &other) != KERN_SUCCESS && !other, "invalid-client-type");
    CHECK(IOServiceOpen(service, mach_task_self(), kNativeFramebufferConnection, &connection) == KERN_SUCCESS, "open-client");
    CHECK(IOServiceOpen(service, mach_task_self(), kNativeFramebufferConnection, &other) != KERN_SUCCESS && !other, "exclusive-client");
    pid_t child = fork();
    CHECK(child >= 0, "permission-fork");
    if (child == 0) {
        if (setgroups(0, NULL) || setgid(20) || setuid(501) || getuid() != 501 || geteuid() != 501) _exit(10);
        // Mach send rights from the parent's connection are not inherited by
        // fork. Discover the service using this child's actual task ports.
        io_service_t child_service;
        if (find_service(&child_service)) _exit(12);
        io_connect_t denied = MACH_PORT_NULL;
        kern_return_t status = IOServiceOpen(child_service, mach_task_self(), kNativeFramebufferConnection, &denied);
        IOObjectRelease(child_service);
        if (status != kIOReturnNotPrivileged || denied)
            fprintf(stderr, "NATIVE_FRAMEBUFFER_PERMISSION:status=%x port=%u\n", status, denied);
        _exit(status == kIOReturnNotPrivileged && !denied ? 0 : 11);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    CHECK(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "root-privilege-enforced");
    NativeFramebufferInfo info;
    size_t size = sizeof(info);
    CHECK(IOConnectCallStructMethod(connection, 9, NULL, 0, &info, &size) != KERN_SUCCESS, "invalid-selector");
    size = sizeof(info) - 1;
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferInformation, NULL, 0, &info, &size) != KERN_SUCCESS, "invalid-info-size");
    size = sizeof(info);
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferInformation, NULL, 0, &info, &size) == KERN_SUCCESS && size == sizeof(info), "read-info");
    printf("NATIVE_FRAMEBUFFER_GEOMETRY:version=%u width=%u height=%u stride=%u depth=%u length=%llu mapping-alignment=%u user-page=%d\n",
           info.version, info.width, info.height, info.row_bytes, info.bits_per_pixel,
           (unsigned long long)info.byte_length, info.mapping_alignment, getpagesize());
    CHECK(info.version == kNativeFramebufferABIVersion && info.bits_per_pixel == 32 &&
          info.width >= 64 && info.height >= 64 && info.width <= UINT32_MAX / 4 &&
          info.row_bytes >= info.width * 4 && info.byte_length == (uint64_t)info.row_bytes * info.height &&
          info.byte_length <= UINT64_C(64) * 1024 * 1024 && getpagesize() > 0 &&
          info.mapping_alignment >= (uint32_t)getpagesize() &&
          info.mapping_alignment % (uint32_t)getpagesize() == 0 &&
          info.byte_length % info.mapping_alignment == 0, "geometry");
    NativeFramebufferPixelLayout layout;
    size = sizeof(layout);
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferPixelLayout, NULL, 0, &layout, &size) == KERN_SUCCESS && size == sizeof(layout), "firmware-pixel-layout");
    CHECK(layout.version == 1 && layout.green_mask == 0xff00 && layout.reserved_mask == 0xff000000 &&
          ((layout.red_mask == 0xff && layout.blue_mask == 0xff0000) ||
           (layout.red_mask == 0xff0000 && layout.blue_mask == 0xff)), "supported-color-masks");
    printf("NATIVE_FRAMEBUFFER_COLOR:red=%08x green=%08x blue=%08x reserved=%08x\n",
           layout.red_mask, layout.green_mask, layout.blue_mask, layout.reserved_mask);
    mach_vm_address_t address = 0;
    mach_vm_size_t length = 0;
    IOOptionBits options = kIOMapAnywhere | kIOMapWriteCombineCache;
    CHECK(IOConnectMapMemory64(connection, 9, mach_task_self(), &address, &length, options) != KERN_SUCCESS, "invalid-memory-type");
    CHECK(IOConnectMapMemory64(connection, kNativeFramebufferMemory, mach_task_self(), &address, &length, options) == KERN_SUCCESS && address && length == info.byte_length, "map-scanout");
    size = 0;
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferReleaseConsole, NULL, 0, NULL, &size) == kIOReturnNotOpen, "release-before-claim-rejected");
    uint32_t invalid_claim = 1;
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferAcquireConsole, &invalid_claim, sizeof(invalid_claim), NULL, &size) == kIOReturnBadArgument,
          "invalid-console-claim-rejected");
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferAcquireConsole, NULL, 0, NULL, &size) == KERN_SUCCESS, "claim-console");
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferAcquireConsole, NULL, 0, NULL, &size) == kIOReturnExclusiveAccess, "duplicate-console-claim-rejected");
    printf("NATIVE_FRAMEBUFFER_CONSOLE_CLAIMED:%s\n", argv[1]);
    volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)address;
    uint32_t left = info.width / 4, top = info.height / 4;
    uint32_t width = info.width / 2, height = info.height / 2, stride = info.row_bytes / 4;
    uint32_t *saved = malloc((size_t)width * height * sizeof(*saved));
    CHECK(saved != NULL, "save-scanout");
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            saved[y * width + x] = pixels[(top + y) * stride + left + x];
            pixels[(top + y) * stride + left + x] = test_pixel(x, width, &layout);
        }
    }
    __sync_synchronize();
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            CHECK(pixels[(top + y) * stride + left + x] == test_pixel(x, width, &layout), "mapped-readback");
    printf("NATIVE_FRAMEBUFFER_READY:%s:%s:%s:%u:%u:%u\n", argv[1], identity.release, identity.machine,
           info.width, info.height, info.row_bytes);
    CHECK(fflush(stdout) == 0, "flush-proof");
    CHECK(getchar() == '\n', "screenshot-acknowledgement");
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            pixels[(top + y) * stride + left + x] = saved[y * width + x];
    __sync_synchronize();
    free(saved);
    CHECK(IOConnectUnmapMemory64(connection, kNativeFramebufferMemory, mach_task_self(), address) == KERN_SUCCESS, "unmap-scanout");
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferReleaseConsole, NULL, 0, NULL, &size) == KERN_SUCCESS, "release-console");
    CHECK(IOConnectCallStructMethod(connection, kNativeFramebufferReleaseConsole, NULL, 0, NULL, &size) == kIOReturnNotOpen, "duplicate-console-release-rejected");
    CHECK(IOServiceClose(connection) == KERN_SUCCESS, "close-client");
    CHECK(IOServiceOpen(service, mach_task_self(), kNativeFramebufferConnection, &other) == KERN_SUCCESS, "reopen-client");
    CHECK(IOConnectCallStructMethod(other, kNativeFramebufferAcquireConsole, NULL, 0, NULL, &size) == KERN_SUCCESS, "reopened-console-claim");
    CHECK(IOServiceClose(other) == KERN_SUCCESS, "close-reopened-client");
    CHECK(IOServiceOpen(service, mach_task_self(), kNativeFramebufferConnection, &other) == KERN_SUCCESS, "reopen-after-close-restored-console");
    CHECK(IOConnectCallStructMethod(other, kNativeFramebufferReleaseConsole, NULL, 0, NULL, &size) == kIOReturnNotOpen, "closed-client-restored-console");
    CHECK(IOServiceClose(other) == KERN_SUCCESS, "close-final-client");
    CHECK(IOObjectRelease(service) == KERN_SUCCESS, "release-service");
    printf("NATIVE_FRAMEBUFFER_OK:%s:%s:%s\n", argv[1], identity.release, identity.machine);
    return 0;
}
