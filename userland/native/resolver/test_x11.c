/* Real native client dylibs, public data APIs, and refused TCP connections.
 * This probe does not implement or stand in for an X server. */
#include <X11/Xlib.h>
#include <X11/Xauth.h>
#include <X11/Xdmcp.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include "native_x11_images.h"

#define API(handle, function) \
    __typeof__(&function) function##_native = (__typeof__(&function))dlsym(handle, #function); \
    if (!function##_native) return __LINE__

int native_x11_check(void)
{
    void *handles[sizeof(native_x11_images) / sizeof(native_x11_images[0])];
    void *xlib = NULL, *xcb = NULL, *xdmcp = NULL, *xau = NULL;
    for (unsigned i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
        fprintf(stderr, "Resolver X11 load: %s\n", native_x11_images[i].name);
        handles[i] = dlopen(native_x11_images[i].path, RTLD_NOW | RTLD_LOCAL);
        if (!handles[i] || !dlsym(handles[i], native_x11_images[i].symbol)) {
            fprintf(stderr, "Native resolver failure: X11 load %s: %s\n", native_x11_images[i].path, dlerror());
            return 1;
        }
        if (!strcmp(native_x11_images[i].name, "libX11")) xlib = handles[i];
        if (!strcmp(native_x11_images[i].name, "libxcb")) xcb = handles[i];
        if (!strcmp(native_x11_images[i].name, "libXdmcp")) xdmcp = handles[i];
        if (!strcmp(native_x11_images[i].name, "libXau")) xau = handles[i];
    }
    if (!xlib || !xcb || !xdmcp || !xau) return 2;
    fprintf(stderr, "Resolver X11 stage: public data APIs\n");
    API(xlib, XInitThreads);
    API(xlib, XStringToKeysym);
    API(xlib, XKeysymToString);
    API(xlib, XOpenDisplay);
    API(xcb, xcb_connect);
    API(xcb, xcb_connection_has_error);
    API(xcb, xcb_disconnect);
    API(xau, XauFileName);
    API(xdmcp, XdmcpWriteCARD8);
    API(xdmcp, XdmcpWriteCARD16);
    API(xdmcp, XdmcpWriteCARD32);
    API(xdmcp, XdmcpReadCARD8);
    API(xdmcp, XdmcpReadCARD16);
    API(xdmcp, XdmcpReadCARD32);
    if (!XInitThreads_native()) return 3;
    if (XStringToKeysym_native("Return") != XK_Return ||
        XStringToKeysym_native("NoSuchNativeKeysym") != NoSymbol) return 4;
    const char *key = XKeysymToString_native(XK_Return);
    if (!key || strcmp(key, "Return")) return 5;
    if (setenv("HOME", "/", 1) || unsetenv("XAUTHORITY")) return 6;
    const char *auth = XauFileName_native();
    if (!auth || strlen(auth) < 11 || strcmp(auth + strlen(auth) - 11, ".Xauthority")) return 6;
    BYTE bytes[7] = {0};
    XdmcpBuffer buffer = {bytes, sizeof(bytes), 0, 0};
    if (!XdmcpWriteCARD8_native(&buffer, 0x42) ||
        !XdmcpWriteCARD16_native(&buffer, 0x1234) ||
        !XdmcpWriteCARD32_native(&buffer, 0x89abcdef)) return 7;
    const unsigned char expected[] = {0x42, 0x12, 0x34, 0x89, 0xab, 0xcd, 0xef};
    if (buffer.pointer != 7 || memcmp(bytes, expected, 7)) return 8;
    if (XdmcpWriteCARD8_native(&buffer, 1)) return 9;
    buffer.count = 7;
    buffer.pointer = 0;
    CARD8 c8; CARD16 c16; CARD32 c32;
    if (!XdmcpReadCARD8_native(&buffer, &c8) || c8 != 0x42 ||
        !XdmcpReadCARD16_native(&buffer, &c16) || c16 != 0x1234 ||
        !XdmcpReadCARD32_native(&buffer, &c32) || c32 != 0x89abcdef ||
        XdmcpReadCARD8_native(&buffer, &c8)) return 10;
    /* Verify the port is unused, then release it before connecting. XNU's
     * tcp_input drops SYNs to a bound TCPS_CLOSED PCB; only the absent-PCB
     * path is a closed-port refusal fixture. No other guest service uses it. */
    fprintf(stderr, "Resolver X11 stage: refused connections\n");
    int reserved = socket(AF_INET, SOCK_STREAM, 0);
    if (reserved < 0) return 11;
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(6099);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(reserved, (struct sockaddr *)&address, sizeof(address))) { close(reserved); return 12; }
    close(reserved);
    int direct = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval timeout = {2, 0};
    if (direct < 0 || setsockopt(direct, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) return 16;
    fprintf(stderr, "Resolver X11 stage: direct native connect\n");
    int connected = connect(direct, (struct sockaddr *)&address, sizeof(address));
    int connection_error = errno;
    close(direct);
    fprintf(stderr, "Resolver X11 native connect: result=%d errno=%d\n", connected, connection_error);
    if (connected != -1 || connection_error != ECONNREFUSED) return 17;
    fprintf(stderr, "Resolver X11 stage: XOpenDisplay\n");
    Display *display = XOpenDisplay_native("127.0.0.1:99.0");
    if (display) return 13;
    fprintf(stderr, "Resolver X11 stage: xcb_connect\n");
    int screen = -1;
    xcb_connection_t *connection = xcb_connect_native("127.0.0.1:99.0", &screen);
    if (!connection || xcb_connection_has_error_native(connection) != XCB_CONN_ERROR) return 14;
    xcb_disconnect_native(connection);
    fprintf(stderr, "Resolver X11 stage: unload\n");
    for (unsigned i = sizeof(handles) / sizeof(handles[0]); i > 0; --i)
        if (dlclose(handles[i - 1])) return 15;
    return 0;
}
