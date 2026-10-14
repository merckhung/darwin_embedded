/*
 * Copyright (c) 2022 Gaël Bonithon <gael@xfce.org>
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

#ifndef __XFW_APPLICATION_WAYLAND_H__
#define __XFW_APPLICATION_WAYLAND_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <glib-object.h>

#include "xfw-application-private.h"
#include "xfw-window-wayland.h"

G_BEGIN_DECLS

#define XFW_TYPE_APPLICATION_WAYLAND (xfw_application_wayland_get_type())
G_DECLARE_FINAL_TYPE(XfwApplicationWayland, xfw_application_wayland, XFW, APPLICATION_WAYLAND, XfwApplication)

typedef struct _XfwApplicationWaylandPrivate XfwApplicationWaylandPrivate;

struct _XfwApplicationWayland {
    XfwApplication parent;
    /*< private >*/
    XfwApplicationWaylandPrivate *priv;
};

XfwApplicationWayland *_xfw_application_wayland_get(XfwWindowWayland *window, const gchar *app_id);
GIcon *_xfw_application_wayland_get_gicon_no_fallback(XfwApplicationWayland *app);

G_END_DECLS

#endif /* __XFW_APPLICATION_WAYLAND_H__ */
