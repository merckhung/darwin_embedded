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

#ifndef __XFW_WORKSPACE_MANAGER_H__
#define __XFW_WORKSPACE_MANAGER_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <glib-object.h>
#include <libxfce4windowing/xfw-workspace-group.h>

G_BEGIN_DECLS

#define XFW_TYPE_WORKSPACE_MANAGER (xfw_workspace_manager_get_type())
G_DECLARE_INTERFACE(XfwWorkspaceManager, xfw_workspace_manager, XFW, WORKSPACE_MANAGER, GObject)

typedef struct _XfwWorkspaceManagerInterface XfwWorkspaceManagerIface;

GList *xfw_workspace_manager_list_workspace_groups(XfwWorkspaceManager *manager);
GList *xfw_workspace_manager_list_workspaces(XfwWorkspaceManager *manager);

G_END_DECLS

#endif /* !__XFW_WORKSPACE_MANAGER_H__ */
