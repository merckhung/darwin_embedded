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

#ifndef __XFW_WORKSPACE_GROUP_H__
#define __XFW_WORKSPACE_GROUP_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <glib-object.h>
#include <libxfce4windowing/xfw-workspace.h>

/* fwd decl */
typedef struct _XfwWorkspaceManager XfwWorkspaceManager;

G_BEGIN_DECLS

#define XFW_TYPE_WORKSPACE_GROUP (xfw_workspace_group_get_type())
G_DECLARE_INTERFACE(XfwWorkspaceGroup, xfw_workspace_group, XFW, WORKSPACE_GROUP, GObject)

#define XFW_TYPE_WORKSPACE_GROUP_CAPABILITIES (xfw_workspace_group_capabilities_get_type())

typedef struct _XfwWorkspaceGroupInterface XfwWorkspaceGroupIface;

/**
 * XfwWorkspaceGroupCapabilities:
 * @XFW_WORKSPACE_GROUP_CAPABILITIES_NONE: group has no capabilities.
 * @XFW_WORKSPACE_GROUP_CAPABILITIES_CREATE_WORKSPACE: new workspaces can be
 *                                                     created in this group.
 * @XFW_WORKSPACE_GROUP_CAPABILITIES_MOVE_VIEWPORT: the viewport coordinates
 *                                                  for this group can be
 *                                                  changed.
 * @XFW_WORKSPACE_GROUP_CAPABILITIES_SET_LAYOUT: the number of rows and columns
 *                                               for this group can be changed.
 *
 * Flags enum representing a bitfield of actions that can be performed on this
 * workspace group.
 **/
typedef enum {
    XFW_WORKSPACE_GROUP_CAPABILITIES_NONE = 0,
    XFW_WORKSPACE_GROUP_CAPABILITIES_CREATE_WORKSPACE = (1 << 0),
    XFW_WORKSPACE_GROUP_CAPABILITIES_MOVE_VIEWPORT = (1 << 1),
    XFW_WORKSPACE_GROUP_CAPABILITIES_SET_LAYOUT = (1 << 2),
} XfwWorkspaceGroupCapabilities;

GType xfw_workspace_group_capabilities_get_type(void);

XfwWorkspaceGroupCapabilities xfw_workspace_group_get_capabilities(XfwWorkspaceGroup *group);
guint xfw_workspace_group_get_workspace_count(XfwWorkspaceGroup *group);
GList *xfw_workspace_group_list_workspaces(XfwWorkspaceGroup *group);
XfwWorkspace *xfw_workspace_group_get_active_workspace(XfwWorkspaceGroup *group);
GList *xfw_workspace_group_get_monitors(XfwWorkspaceGroup *group);
XfwWorkspaceManager *xfw_workspace_group_get_workspace_manager(XfwWorkspaceGroup *group);

gboolean xfw_workspace_group_create_workspace(XfwWorkspaceGroup *group, const gchar *name, GError **error);
gboolean xfw_workspace_group_move_viewport(XfwWorkspaceGroup *group, gint x, gint y, GError **error);
gboolean xfw_workspace_group_set_layout(XfwWorkspaceGroup *group, gint rows, gint columns, GError **error);

G_END_DECLS

#endif /* !__XFW_WORKSPACE_GROUP_H__ */
