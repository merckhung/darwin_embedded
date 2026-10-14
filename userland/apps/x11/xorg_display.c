/* Native Darwin Xorg display: real IOUserClient scanout and fixed-mode RandR. */
#include "dix-config.h"
#include "NativeBootFramebuffer.h"
#include <NativeIOKit.h>
#include <NativeIOKitMIG.h>
#include <IOKit/IOMapTypes.h>
#include <mach/mach_host.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include "scrnintstr.h"
#include "servermd.h"
#define PSZ 8
#include "fb.h"
#include "mi.h"
#include "micmap.h"
#include "mipointer.h"
#include "randrstr.h"
#include "os.h"

static io_connect_t display_connection;
static mach_vm_address_t display_address;
static mach_vm_size_t display_length;
static NativeFramebufferInfo display_info;
static NativeFramebufferPixelLayout display_layout;
static Bool display_claimed;
static CloseScreenProcPtr framebuffer_close;
static RRModePtr native_mode;
static RRCrtcPtr native_crtc;
static RROutputPtr native_output;

static void close_display(void)
{
    if (display_address) {
        if (IOConnectUnmapMemory64(display_connection, kNativeFramebufferMemory, mach_task_self(), display_address) != KERN_SUCCESS)
            ErrorF("Xorg: native framebuffer unmap failed\n");
        display_address = 0;
    }
    if (display_claimed) {
        size_t size = 0;
        if (IOConnectCallStructMethod(display_connection, kNativeFramebufferReleaseConsole, NULL, 0, NULL, &size) != KERN_SUCCESS)
            ErrorF("Xorg: native console release failed\n");
        display_claimed = FALSE;
    }
    if (display_connection) {
        if (IOServiceClose(display_connection) != KERN_SUCCESS) ErrorF("Xorg: native framebuffer close failed\n");
        display_connection = MACH_PORT_NULL;
    }
}

static Bool open_display(void)
{
    mach_port_t host = mach_host_self(), main_port = MACH_PORT_NULL;
    kern_return_t status = host_get_io_main(host, &main_port);
    mach_port_deallocate(mach_task_self(), host);
    if (status != KERN_SUCCESS || !main_port) return FALSE;
    io_string_t match = "<dict><key>IOProviderClass</key><string>NativeBootFramebuffer</string></dict>";
    io_iterator_t iterator = MACH_PORT_NULL;
    kern_return_t matching = KERN_FAILURE;
    status = io_service_get_matching_services_ool(main_port, match, (mach_msg_type_number_t)strlen(match) + 1, &matching, &iterator);
    mach_port_deallocate(mach_task_self(), main_port);
    if (status != KERN_SUCCESS || matching != KERN_SUCCESS || !iterator) return FALSE;
    io_service_t service = IOIteratorNext(iterator), additional = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (additional) IOObjectRelease(additional);
    if (!service || additional) { if (service) IOObjectRelease(service); return FALSE; }
    status = IOServiceOpen(service, mach_task_self(), kNativeFramebufferConnection, &display_connection);
    IOObjectRelease(service);
    if (status != KERN_SUCCESS) return FALSE;
    size_t size = sizeof(display_info);
    if (IOConnectCallStructMethod(display_connection, kNativeFramebufferInformation, NULL, 0, &display_info, &size) != KERN_SUCCESS || size != sizeof(display_info)) goto failed;
    size = sizeof(display_layout);
    if (IOConnectCallStructMethod(display_connection, kNativeFramebufferPixelLayout, NULL, 0, &display_layout, &size) != KERN_SUCCESS || size != sizeof(display_layout)) goto failed;
    if (display_info.version != 1 || display_info.bits_per_pixel != 32 || !display_info.width || !display_info.height ||
        display_info.width > 32767 || display_info.height > 32767 || display_info.row_bytes < display_info.width * 4 || (display_info.row_bytes & 3) ||
        display_info.byte_length != (uint64_t)display_info.row_bytes * display_info.height || display_info.byte_length > UINT64_C(64) * 1024 * 1024 ||
        display_layout.version != 1 || display_layout.green_mask != 0xff00 || display_layout.reserved_mask != 0xff000000 ||
        !((display_layout.red_mask == 0xff0000 && display_layout.blue_mask == 0xff) || (display_layout.red_mask == 0xff && display_layout.blue_mask == 0xff0000))) goto failed;
    status = IOConnectMapMemory64(display_connection, kNativeFramebufferMemory, mach_task_self(), &display_address, &display_length,
                                 kIOMapAnywhere | kIOMapWriteCombineCache);
    if (status != KERN_SUCCESS || !display_address || display_length != display_info.byte_length) goto failed;
    size = 0;
    if (IOConnectCallStructMethod(display_connection, kNativeFramebufferAcquireConsole, NULL, 0, NULL, &size) != KERN_SUCCESS) goto failed;
    display_claimed = TRUE;
    return TRUE;
failed:
    close_display();
    return FALSE;
}

static Bool fixed_mode_info(ScreenPtr screen, Rotation *rotations)
{
    (void)screen;
    *rotations = RR_Rotate_0;
    return TRUE;
}

static Bool fixed_screen_size(ScreenPtr screen, CARD16 width, CARD16 height, CARD32 mm_width, CARD32 mm_height)
{
    if (width != display_info.width || height != display_info.height || !mm_width || !mm_height) return FALSE;
    screen->mmWidth = mm_width; screen->mmHeight = mm_height;
    RRScreenSizeNotify(screen);
    return TRUE;
}

static Bool fixed_crtc_set(ScreenPtr screen, RRCrtcPtr crtc, RRModePtr mode, int x, int y, Rotation rotation, int count, RROutputPtr *outputs)
{
    (void)screen;
    if (crtc != native_crtc || mode != native_mode || x || y || rotation != RR_Rotate_0 || count != 1 || !outputs || outputs[0] != native_output) return FALSE;
    return RRCrtcNotify(crtc, mode, 0, 0, RR_Rotate_0, NULL, count, outputs);
}

static Bool fixed_mode_validate(ScreenPtr screen, RROutputPtr output, RRModePtr mode)
{
    (void)screen;
    return output == native_output && mode == native_mode;
}

static Bool init_randr(ScreenPtr screen)
{
    if (!RRScreenInit(screen)) return FALSE;
    rrScrPrivPtr state = rrGetScrPriv(screen);
    state->rrGetInfo = fixed_mode_info; state->rrCrtcSet = fixed_crtc_set;
    state->rrScreenSetSize = fixed_screen_size; state->rrOutputValidateMode = fixed_mode_validate;
    RRScreenSetSizeRange(screen, display_info.width, display_info.height, display_info.width, display_info.height);
    char name[32]; snprintf(name, sizeof(name), "%ux%u", display_info.width, display_info.height);
    xRRModeInfo mode = {0}; mode.width = display_info.width; mode.height = display_info.height; mode.nameLength = strlen(name);
    native_mode = RRModeGet(&mode, name);
    native_crtc = RRCrtcCreate(screen, NULL);
    native_output = RROutputCreate(screen, "NativeBootFramebuffer", 21, NULL);
    if (!native_mode || !native_crtc || !native_output || !RROutputSetClones(native_output, NULL, 0) ||
        !RROutputSetModes(native_output, &native_mode, 1, 1) || !RROutputSetCrtcs(native_output, &native_crtc, 1) ||
        !RROutputSetConnection(native_output, RR_Connected)) return FALSE;
    return RRCrtcNotify(native_crtc, native_mode, 0, 0, RR_Rotate_0, NULL, 1, &native_output);
}

static Bool cursor_offscreen(ScreenPtr *screen, int *x, int *y)
{
    (void)screen; (void)x; (void)y;
    return FALSE; /* The fixed native scanout has no adjacent screen. */
}

static void cursor_crossscreen(ScreenPtr screen, Bool entering)
{
    (void)screen; (void)entering; /* The MI software cursor needs no hardware action. */
}

static miPointerScreenFuncRec cursor_functions = {cursor_offscreen, cursor_crossscreen, miPointerWarpCursor};

static Bool close_screen(ScreenPtr screen)
{
    screen->CloseScreen = framebuffer_close;
    if (screen->devPrivate) screen->DestroyPixmap(screen->devPrivate);
    screen->devPrivate = NULL;
    Bool result = framebuffer_close(screen);
    close_display();
    native_mode = NULL; native_crtc = NULL; native_output = NULL;
    return result;
}

static Bool init_screen(ScreenPtr screen, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!miSetVisualTypesAndMasks(24, 1 << TrueColor, 8, TrueColor, display_layout.red_mask, display_layout.green_mask, display_layout.blue_mask)) return FALSE;
    miSetPixmapDepths();
    int dpi = monitorResolution ? monitorResolution : 96;
    if (!fbScreenInit(screen, (void *)(uintptr_t)display_address, display_info.width, display_info.height, dpi, dpi, display_info.row_bytes / 4, 32) ||
        !fbPictureInit(screen, NULL, 0) || !init_randr(screen) || !miDCInitialize(screen, &cursor_functions) || !fbCreateDefColormap(screen)) return FALSE;
    screen->blackPixel = 0; screen->whitePixel = display_layout.red_mask | display_layout.green_mask | display_layout.blue_mask;
    framebuffer_close = screen->CloseScreen; screen->CloseScreen = close_screen;
    ErrorF("Xorg native scanout: %ux%u stride %u, actual IOUserClient mapping\n", display_info.width, display_info.height, display_info.row_bytes);
    return TRUE;
}

void InitOutput(ScreenInfo *info, int argc, char **argv)
{
    if (!open_display()) FatalError("Xorg: native framebuffer service, format, mapping or console ownership unavailable\n");
    static const int depths[] = {1, 4, 8, 16, 24, 32};
    for (unsigned index = 0; index < sizeof(depths) / sizeof(depths[0]); ++index) {
        info->formats[index].depth = depths[index];
        info->formats[index].bitsPerPixel = depths[index] == 1 ? 1 : depths[index] <= 8 ? 8 : depths[index] == 16 ? 16 : 32;
        info->formats[index].scanlinePad = BITMAP_SCANLINE_PAD;
    }
    info->numPixmapFormats = sizeof(depths) / sizeof(depths[0]);
    info->imageByteOrder = IMAGE_BYTE_ORDER; info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    info->bitmapScanlinePad = BITMAP_SCANLINE_PAD; info->bitmapBitOrder = BITMAP_BIT_ORDER;
    if (AddScreen(init_screen, argc, argv) == -1) { close_display(); FatalError("Xorg: native screen initialization failed\n"); }
}

void OsVendorInit(void) { /* Paths and authentication use the original POSIX OS layer. */ }
void OsVendorFatalError(const char *format, va_list arguments) { (void)format; (void)arguments; close_display(); }
void ddxGiveUp(enum ExitCode error) { (void)error; close_display(); }
int ddxProcessArgument(int argc, char **argv, int index) { (void)argc; (void)argv; (void)index; return 0; }
void ddxUseMsg(void) { ErrorF("Native Darwin display uses the current firmware framebuffer and fixed mode.\n"); }
