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

#ifndef __XFW_WORKSPACE_PRIVATE_H__
#define __XFW_WORKSPACE_PRIVATE_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include "xfw-workspace.h"

G_BEGIN_DECLS


struct _XfwWorkspaceInterface {
    /*< private >*/
    GTypeInterface g_iface;

    /*< public >*/

    /* Signals */
    void (*name_changed)(XfwWorkspace *workspace);
    void (*capabilities_changed)(XfwWorkspace *workspace, XfwWorkspaceCapabilities changed_mask, XfwWorkspaceCapabilities new_capabilities);
    void (*state_changed)(XfwWorkspace *workspace, XfwWorkspaceState changed_mask, XfwWorkspaceState new_state);
    void (*group_changed)(XfwWorkspace *workspace, XfwWorkspaceGroup *previous_group);

    /* Virtual Table */
    const gchar *(*get_id)(XfwWorkspace *workspace);
    const gchar *(*get_name)(XfwWorkspace *workspace);
    XfwWorkspaceCapabilities (*get_capabilities)(XfwWorkspace *workspace);
    XfwWorkspaceState (*get_state)(XfwWorkspace *workspace);
    guint (*get_number)(XfwWorkspace *workspace);
    XfwWorkspaceGroup *(*get_workspace_group)(XfwWorkspace *workspace);

    gint (*get_layout_row)(XfwWorkspace *workspace);
    gint (*get_layout_column)(XfwWorkspace *workspace);
    XfwWorkspace *(*get_neighbor)(XfwWorkspace *workspace, XfwDirection direction);
    GdkRectangle *(*get_geometry)(XfwWorkspace *workspace);

    gboolean (*activate)(XfwWorkspace *workspace, GError **error);
    gboolean (*remove)(XfwWorkspace *workspace, GError **error);
    gboolean (*assign_to_workspace_group)(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error);
};

G_END_DECLS

#endif /* !__XFW_WORKSPACE_PRIVATE_H__ */
