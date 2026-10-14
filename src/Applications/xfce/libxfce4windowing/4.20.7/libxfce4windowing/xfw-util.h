/*
 * Copyright (c) 2022 Brian Tarricone <brian@tarricone.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#ifndef __XFW_UTIL_H__
#define __XFW_UTIL_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

/**
 * XFW_ERROR:
 *
 * The error domain for all errors returned by this library.
 **/
#define XFW_ERROR (xfw_error_quark())

G_BEGIN_DECLS

/**
 * XfwError:
 * @XFW_ERROR_UNSUPPORTED: the operation attempted is not supported.
 * @XFW_ERROR_INTERNAL: an internal error has occurred.
 *
 * An error code enum describing possible errors returned by this library.
 **/
typedef enum _XfwError {
    XFW_ERROR_UNSUPPORTED = 0,
    XFW_ERROR_INTERNAL,
} XfwError;

/**
 * XfwWindowing:
 * @XFW_WINDOWING_UNKNOWN: the application is running under an unknown
 *                         or unsupported windowing system.
 * @XFW_WINDOWING_X11: the application is running under an X11 server.
 * @XFW_WINDOWING_WAYLAND: the application is running under a Wayland
 *                         comopositor.
 *
 * Represents the windowing environment that is currently running.  Note that
 * for an application running on XWayland, this will return #XFW_WINDOWING_X11.
 **/
typedef enum {
    XFW_WINDOWING_UNKNOWN = 0,
    XFW_WINDOWING_X11,
    XFW_WINDOWING_WAYLAND,
} XfwWindowing;

/**
 * XfwDirection:
 * @XFW_DIRECTION_UP: upward from the current location
 * @XFW_DIRECTION_DOWN: downward from the current location
 * @XFW_DIRECTION_LEFT: to the left of the current location
 * @XFW_DIRECTION_RIGHT: to the right of the current location
 *
 * Represents a direction, either of position ("This workspace is to the left
 * of the current workspace") or movement ("Move this window to the workspace to
 * the right of its current workspace").
 **/
typedef enum {
    XFW_DIRECTION_UP = 0,
    XFW_DIRECTION_DOWN,
    XFW_DIRECTION_LEFT,
    XFW_DIRECTION_RIGHT,
} XfwDirection;

/**
 * XfwClientType:
 * @XFW_CLIENT_TYPE_APPLICATION: a regular application
 * @XFW_CLIENT_TYPE_PAGER: a pager or other user-controlled desktop component
 *
 * The type of the application.
 *
 * See #xfw_set_client_type() for details.
 *
 * Since: 4.19.3
 **/
typedef enum {
    XFW_CLIENT_TYPE_APPLICATION = 1,
    XFW_CLIENT_TYPE_PAGER = 2,
} XfwClientType;

GQuark xfw_error_quark(void);
GType xfw_direction_get_type(void);

XfwWindowing xfw_windowing_get(void);

void xfw_set_client_type(XfwClientType client_type);

/**
 * xfw_windowing_error_trap_push:
 * @display: a #GdkDisplay.
 *
 * Traps errors in the underlying windowing environment.  Error traps work as
 * a stack, so for every "push" call, there needs to be a "pop" call.  Multiple
 * pushes need to be matched with an equal number of pops.
 *
 * This only does anything on X11.
 **/
static inline void
xfw_windowing_error_trap_push(GdkDisplay *display) {
#ifdef GDK_WINDOWING_X11
    if (xfw_windowing_get() == XFW_WINDOWING_X11) {
        gdk_x11_display_error_trap_push(display);
    }
#endif /* GDK_WINDOWING_X11 */
}

/**
 * xfw_windowing_error_trap_pop:
 * @display: a #GdkDisplay.
 *
 * Pops the topmost error trap off of the stack.
 *
 * This only does anything on X11.
 *
 * Return value: Returns the error code of the error that occured, or `0` if
 * there was no error.
 **/
static inline gint
xfw_windowing_error_trap_pop(GdkDisplay *display) {
#ifdef GDK_WINDOWING_X11
    if (xfw_windowing_get() == XFW_WINDOWING_X11) {
        return gdk_x11_display_error_trap_pop(display);
    } else
#endif /* GDK_WINDOWING_X11 */
    {
        return 0;
    }
}

/**
 * xfw_windowing_error_trap_pop_ignored:
 * @display: a #GdkDisplay.
 *
 * Pops the topmost error trap off of the stack.
 *
 * This only does anything on X11.
 **/
static inline void
xfw_windowing_error_trap_pop_ignored(GdkDisplay *display) {
#ifdef GDK_WINDOWING_X11
    if (xfw_windowing_get() == XFW_WINDOWING_X11) {
        gdk_x11_display_error_trap_pop_ignored(display);
    }
#endif /* GDK_WINDOWING_X11 */
}

G_END_DECLS

#endif /* __XFW_UTIL_H__ */
