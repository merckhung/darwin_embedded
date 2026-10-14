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

#ifndef __XFW_WORKSPACE_GROUP_PRIVATE_H__
#define __XFW_WORKSPACE_GROUP_PRIVATE_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include "xfw-monitor.h"
#include "xfw-workspace-group.h"
#include "xfw-workspace-manager.h"

G_BEGIN_DECLS

struct _XfwWorkspaceGroupInterface {
    /*< private >*/
    GTypeInterface g_iface;

    /*< public >*/

    /* Signals */
    void (*capabilities_changed)(XfwWorkspaceGroup *group,
                                 XfwWorkspaceGroupCapabilities changed_mask,
                                 XfwWorkspaceGroupCapabilities new_capabilities);
    void (*active_workspace_changed)(XfwWorkspaceGroup *group,
                                     XfwWorkspace *previously_active_workspace);
    void (*monitor_added)(XfwWorkspaceGroup *group,
                          XfwMonitor *monitor);
    void (*monitor_removed)(XfwWorkspaceGroup *group,
                            XfwMonitor *monitor);
    void (*monitors_changed)(XfwWorkspaceGroup *group);
    void (*viewports_changed)(XfwWorkspaceGroup *group);
    void (*workspace_added)(XfwWorkspaceGroup *group,
                            XfwWorkspace *workspace);
    void (*workspace_removed)(XfwWorkspaceGroup *group,
                              XfwWorkspace *workspace);

    /* Virtual Table */
    XfwWorkspaceGroupCapabilities (*get_capabilities)(XfwWorkspaceGroup *group);
    guint (*get_workspace_count)(XfwWorkspaceGroup *group);
    GList *(*list_workspaces)(XfwWorkspaceGroup *group);
    XfwWorkspace *(*get_active_workspace)(XfwWorkspaceGroup *group);
    GList *(*get_monitors)(XfwWorkspaceGroup *group);
    XfwWorkspaceManager *(*get_workspace_manager)(XfwWorkspaceGroup *group);

    gboolean (*create_workspace)(XfwWorkspaceGroup *group, const gchar *name, GError **error);
    gboolean (*move_viewport)(XfwWorkspaceGroup *group, gint x, gint y, GError **error);
    gboolean (*set_layout)(XfwWorkspaceGroup *group, gint rows, gint columns, GError **error);
};

G_END_DECLS

#endif /* !__XFW_WORKSPACE_GROUP_PRIVATE_H__ */
