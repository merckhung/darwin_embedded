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

#ifndef __XFW_WINDOW_WAYLAND_H__
#define __XFW_WINDOW_WAYLAND_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <glib-object.h>
#include <stdint.h>

#include "protocols/wlr-foreign-toplevel-management-unstable-v1-client.h"
#include "protocols/xfce-foreign-toplevel-management-private-v1-client.h"

#include "xfw-window-private.h"

G_BEGIN_DECLS

#define XFW_TYPE_WINDOW_WAYLAND (xfw_window_wayland_get_type())
G_DECLARE_FINAL_TYPE(XfwWindowWayland, xfw_window_wayland, XFW, WINDOW_WAYLAND, XfwWindow)

typedef struct _XfwWindowWaylandPrivate XfwWindowWaylandPrivate;

struct _XfwWindowWayland {
    XfwWindow parent;
    /*< private >*/
    XfwWindowWaylandPrivate *priv;
};

typedef struct _IconSize {
    uint32_t size;
    uint32_t scale;
} IconSize;

struct zwlr_foreign_toplevel_handle_v1 *_xfw_window_wayland_get_wlr_handle(XfwWindowWayland *window);
struct xfce_foreign_toplevel_handle_v1 *_xfw_window_wayland_get_xfce_handle(XfwWindowWayland *window);
GList *_xfw_window_wayland_get_icon_sizes(XfwWindowWayland *window);

G_END_DECLS

#endif /* __XFW_WINDOW_WAYLAND_H__ */
