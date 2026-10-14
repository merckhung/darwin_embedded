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

#ifndef __XFW_WORKSPACE_GROUP_DUMMY_H__
#define __XFW_WORKSPACE_GROUP_DUMMY_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <glib-object.h>

#include "xfw-workspace-group.h"
#include "xfw-workspace.h"

G_BEGIN_DECLS

#define XFW_TYPE_WORKSPACE_GROUP_DUMMY (xfw_workspace_group_dummy_get_type())
G_DECLARE_FINAL_TYPE(XfwWorkspaceGroupDummy, xfw_workspace_group_dummy, XFW, WORKSPACE_GROUP_DUMMY, GObject)

typedef struct _XfwWorkspaceGroupDummyPrivate XfwWorkspaceGroupDummyPrivate;

typedef gboolean (*XfwCreateWorkspaceFunc)(XfwWorkspaceGroup *group, const gchar *name, GError **error);
typedef gboolean (*XfwMoveViewportFunc)(XfwWorkspaceGroup *group, gint x, gint y, GError **error);
typedef gboolean (*XfwSetLayoutFunc)(XfwWorkspaceGroup *group, gint rows, gint columns, GError **error);

struct _XfwWorkspaceGroupDummy {
    GObject parent;
    /*< private >*/
    XfwWorkspaceGroupDummyPrivate *priv;
};

void _xfw_workspace_group_dummy_set_workspaces(XfwWorkspaceGroupDummy *group, GList *workspaces);
void _xfw_workspace_group_dummy_set_active_workspace(XfwWorkspaceGroupDummy *group, XfwWorkspace *workspace);

G_END_DECLS

#endif /* __XFW_WORKSPACE_GROUP_DUMMY_H__ */
