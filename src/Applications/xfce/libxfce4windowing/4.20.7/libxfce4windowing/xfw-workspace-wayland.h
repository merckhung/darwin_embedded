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

#ifndef __XFW_WORKSPACE_WAYLAND_H__
#define __XFW_WORKSPACE_WAYLAND_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <glib-object.h>

#include "protocols/ext-workspace-v1-client.h"

#include "xfw-workspace-group.h"

G_BEGIN_DECLS

#define XFW_TYPE_WORKSPACE_WAYLAND (xfw_workspace_wayland_get_type())
G_DECLARE_FINAL_TYPE(XfwWorkspaceWayland, xfw_workspace_wayland, XFW, WORKSPACE_WAYLAND, GObject)

typedef struct _XfwWorkspaceWaylandPrivate XfwWorkspaceWaylandPrivate;

struct _XfwWorkspaceWayland {
    GObject parent;
    /*< private >*/
    XfwWorkspaceWaylandPrivate *priv;
};

struct ext_workspace_handle_v1 *_xfw_workspace_wayland_get_handle(XfwWorkspaceWayland *workspace);
void _xfw_workspace_wayland_set_number(XfwWorkspaceWayland *workspace, guint number);
void _xfw_workspace_wayland_set_workspace_group(XfwWorkspaceWayland *workspace, XfwWorkspaceGroup *group);
void _xfw_workspace_wayland_set_workspace_manager_handle(XfwWorkspaceWayland *workspace, struct ext_workspace_manager_v1 *manager_handle);

G_END_DECLS

#endif /* __XFW_WORKSPACE_WAYLAND_H__ */
