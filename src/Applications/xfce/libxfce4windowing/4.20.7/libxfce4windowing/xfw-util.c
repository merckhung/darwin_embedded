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

/**
 * SECTION:xfw-util
 * @title: Utilities
 * @short_description: Miscellaneous windowing utilities
 * @stability: Unstable
 * @include: libxfce4windowing/libxfce4windowing.h
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gdk/gdk.h>
#ifdef ENABLE_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef ENABLE_X11
#include <gdk/gdkx.h>
#include <libwnck/libwnck.h>
#endif

#include "libxfce4windowing-private.h"
#include "xfw-util.h"
#include "libxfce4windowing-visibility.h"

G_DEFINE_ENUM_TYPE(XfwDirection, xfw_direction,
                   G_DEFINE_ENUM_VALUE(XFW_DIRECTION_UP, "up"),
                   G_DEFINE_ENUM_VALUE(XFW_DIRECTION_DOWN, "down"),
                   G_DEFINE_ENUM_VALUE(XFW_DIRECTION_LEFT, "left"),
                   G_DEFINE_ENUM_VALUE(XFW_DIRECTION_RIGHT, "right"))

/**
 * xfw_windowing_get:
 *
 * Determines the windowing environment that is currently active.
 *
 * Return value: A value from the #XfwWindowing enum.
 **/
XfwWindowing
xfw_windowing_get(void) {
    static XfwWindowing windowing = XFW_WINDOWING_UNKNOWN;

    if (G_UNLIKELY(windowing == XFW_WINDOWING_UNKNOWN)) {
        GdkDisplay *gdpy = gdk_display_get_default();

        _libxfce4windowing_init();

#ifdef ENABLE_X11
        if (GDK_IS_X11_DISPLAY(gdpy)) {
            windowing = XFW_WINDOWING_X11;
        } else
#endif /* ENABLE_X11 */
#ifdef ENABLE_WAYLAND
            if (GDK_IS_WAYLAND_DISPLAY(gdpy))
        {
            windowing = XFW_WINDOWING_WAYLAND;
        } else
#endif /* ENABLE_WAYLAND */
        {
            g_critical("Unknown/unsupported GDK windowing type");
        }
    }

    return windowing;
}

/**
 * xfw_set_client_type:
 * @client_type: A #XfwClientType
 *
 * Sets the type of the application.  This is used when sending various
 * messages to control the behavior of other windows, to indicate the source of
 * the control.  In general, #XFW_CLIENT_TYPE_APPLICATION will be interpreted
 * as automated control from a regular application, and #XFW_CLIENT_TYPE_PAGER
 * will be interpreted as user-initiated control from a desktop component
 * application like a pager or dock.
 *
 * This does nothing on Wayland, but is safe to call under a Wayland session.
 *
 * Since: 4.19.3
 **/
void
xfw_set_client_type(XfwClientType client_type) {
#ifdef ENABLE_X11
    if (xfw_windowing_get() == XFW_WINDOWING_X11) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        wnck_set_client_type((WnckClientType)client_type);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
#endif
}

GQuark
xfw_error_quark(void) {
    static GQuark quark = 0;
    if (quark == 0) {
        quark = g_quark_from_static_string("xfw-error-quark");
    }
    return quark;
}

#define __XFW_UTIL_C__
#include "libxfce4windowing-visibility.c"
