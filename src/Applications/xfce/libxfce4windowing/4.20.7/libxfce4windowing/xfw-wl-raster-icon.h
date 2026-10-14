/*
 * Copyright (c) 2026 Brian Tarricone <brian@tarricone.org>
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

#ifndef __XFW_WL_RASTER_ICON_H__
#define __XFW_WL_RASTER_ICON_H__

#include <glib-object.h>

#include "xfw-window-wayland.h"

G_BEGIN_DECLS

#define XFW_TYPE_WL_RASTER_ICON (xfw_wl_raster_icon_get_type())
G_DECLARE_FINAL_TYPE(XfwWlRasterIcon, xfw_wl_raster_icon, XFW, WL_RASTER_ICON, GObject)

XfwWlRasterIcon *_xfw_wl_raster_icon_new(XfwWindowWayland *window);

G_END_DECLS

#endif /* __XFW_WL_RASTER_ICON_H__ */
