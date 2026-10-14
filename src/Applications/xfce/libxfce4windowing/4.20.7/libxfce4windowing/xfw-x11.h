/*
 * Copyright (c) 2024 Brian Tarricone <brian@tarricone.org>
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

#ifndef __XFW_X11_H__
#define __XFW_X11_H__

/**
 * SECTION:xfw-x11
 * @title: X11 Backend
 * @short_description: Functionality specific to the X11 windowing system
 * @stability: Unstable
 * @include: libxfce4windowing/xfw-x11.h
 *
 * This file includes functionality specific to the X11 windowing system, if
 * libxfce4windowing does not provide all the functionality an application
 * needs.
 **/

#include <X11/X.h>
#include <glib.h>
#include <libxfce4windowing/libxfce4windowing.h>

G_BEGIN_DECLS

Window xfw_window_x11_get_xid(XfwWindow *window);

G_END_DECLS

#endif
