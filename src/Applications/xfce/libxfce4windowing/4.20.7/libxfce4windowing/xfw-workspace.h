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

#ifndef __XFW_WORKSPACE_H__
#define __XFW_WORKSPACE_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <glib-object.h>
#include <libxfce4windowing/xfw-util.h>

G_BEGIN_DECLS

/* fwd decl */
typedef struct _XfwWorkspaceGroup XfwWorkspaceGroup;

#define XFW_TYPE_WORKSPACE (xfw_workspace_get_type())
G_DECLARE_INTERFACE(XfwWorkspace, xfw_workspace, XFW, WORKSPACE, GObject)

#define XFW_TYPE_WORKSPACE_CAPABILITIES (xfw_workspace_capabilities_get_type())
#define XFW_TYPE_WORKSPACE_STATE (xfw_workspace_state_get_type())

typedef struct _XfwWorkspaceInterface XfwWorkspaceIface;

/**
 * XfwWorkspaceCapabilities:
 * @XFW_WORKSPACE_CAPABILITIES_NONE: workspace has no capabilities.
 * @XFW_WORKSPACE_CAPABILITIES_ACTIVATE: workspace can be activated.
 * @XFW_WORKSPACE_CAPABILITIES_DEACTIVATE: workspace can be deactivated.
 * @XFW_WORKSPACE_CAPABILITIES_REMOVE: workspace can be removed.
 * @XFW_WORKSPACE_CAPABILITIES_ASSIGN: workspace can be assigned to a group.
 *
 * Flags enum representing a bitfield of actions that can be performed on this
 * workspace.
 **/
typedef enum {
    XFW_WORKSPACE_CAPABILITIES_NONE = 0,
    XFW_WORKSPACE_CAPABILITIES_ACTIVATE = (1 << 0),
    XFW_WORKSPACE_CAPABILITIES_DEACTIVATE = (1 << 1),
    XFW_WORKSPACE_CAPABILITIES_REMOVE = (1 << 2),
    XFW_WORKSPACE_CAPABILITIES_ASSIGN = (1 << 3)
} XfwWorkspaceCapabilities;

/**
 * XfwWorkspaceState:
 * @XFW_WORKSPACE_STATE_NONE: workspace has no state information.
 * @XFW_WORKSPACE_STATE_ACTIVE: workspace is the active workspace in its group.
 * @XFW_WORKSPACE_STATE_URGENT: workspace contains a window that is requesting
 *                              attention.
 * @XFW_WORKSPACE_STATE_HIDDEN: workspace should be hidden from pagers or other
 *                              UI elements.
 * @XFW_WORKSPACE_STATE_VIRTUAL: workspace has a valid, visible viewport.
 *
 * Flags enum representing a bitfield that describes the workspace's state.
 **/
typedef enum {
    XFW_WORKSPACE_STATE_NONE = 0,
    XFW_WORKSPACE_STATE_ACTIVE = (1 << 0),
    XFW_WORKSPACE_STATE_URGENT = (1 << 1),
    XFW_WORKSPACE_STATE_HIDDEN = (1 << 2),
    XFW_WORKSPACE_STATE_VIRTUAL = (1 << 3),
} XfwWorkspaceState;

GType xfw_workspace_capabilities_get_type(void);
GType xfw_workspace_state_get_type(void);

const gchar *xfw_workspace_get_id(XfwWorkspace *workspace);
const gchar *xfw_workspace_get_name(XfwWorkspace *workspace);
XfwWorkspaceCapabilities xfw_workspace_get_capabilities(XfwWorkspace *workspace);
XfwWorkspaceState xfw_workspace_get_state(XfwWorkspace *workspace);
guint xfw_workspace_get_number(XfwWorkspace *workspace);
XfwWorkspaceGroup *xfw_workspace_get_workspace_group(XfwWorkspace *workspace);

gint xfw_workspace_get_layout_row(XfwWorkspace *workspace);
gint xfw_workspace_get_layout_column(XfwWorkspace *workspace);
XfwWorkspace *xfw_workspace_get_neighbor(XfwWorkspace *workspace, XfwDirection direction);
GdkRectangle *xfw_workspace_get_geometry(XfwWorkspace *workspace);

gboolean xfw_workspace_activate(XfwWorkspace *workspace, GError **error);
gboolean xfw_workspace_remove(XfwWorkspace *workspace, GError **error);

gboolean xfw_workspace_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error);

G_END_DECLS

#endif /* !__XFW_WORKSPACE_H__ */
