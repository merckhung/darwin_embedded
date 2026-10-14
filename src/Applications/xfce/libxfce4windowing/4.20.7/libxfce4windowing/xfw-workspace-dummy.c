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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib/gi18n-lib.h>

#include "libxfce4windowing-private.h"
#include "xfw-util.h"
#include "xfw-workspace-dummy.h"
#include "xfw-workspace-private.h"

struct _XfwWorkspaceDummyPrivate {
    XfwWorkspaceGroup *group;
    GdkRectangle geometry;
};

static void xfw_workspace_dummy_workspace_init(XfwWorkspaceIface *iface);
static void xfw_workspace_dummy_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_workspace_dummy_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static const gchar *xfw_workspace_dummy_get_id(XfwWorkspace *workspace);
static const gchar *xfw_workspace_dummy_get_name(XfwWorkspace *workspace);
static XfwWorkspaceCapabilities xfw_workspace_dummy_get_capabilities(XfwWorkspace *workspace);
static XfwWorkspaceState xfw_workspace_dummy_get_state(XfwWorkspace *workspace);
static guint xfw_workspace_dummy_get_number(XfwWorkspace *workspace);
static XfwWorkspaceGroup *xfw_workspace_dummy_get_workspace_group(XfwWorkspace *workspace);
gint xfw_workspace_dummy_get_layout_row(XfwWorkspace *workspace);
gint xfw_workspace_dummy_get_layout_column(XfwWorkspace *workspace);
XfwWorkspace *xfw_workspace_dummy_get_neighbor(XfwWorkspace *workspace, XfwDirection direction);
static GdkRectangle *xfw_workspace_dummy_get_geometry(XfwWorkspace *workspace);
static gboolean xfw_workspace_dummy_activate(XfwWorkspace *workspace, GError **error);
static gboolean xfw_workspace_dummy_remove(XfwWorkspace *workspace, GError **error);
static gboolean xfw_workspace_dummy_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error);

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWorkspaceDummy, xfw_workspace_dummy, G_TYPE_OBJECT,
                              G_ADD_PRIVATE(XfwWorkspaceDummy)
                              G_IMPLEMENT_INTERFACE(XFW_TYPE_WORKSPACE,
                                                    xfw_workspace_dummy_workspace_init))

static void
xfw_workspace_dummy_class_init(XfwWorkspaceDummyClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    gklass->set_property = xfw_workspace_dummy_set_property;
    gklass->get_property = xfw_workspace_dummy_get_property;
    _xfw_workspace_install_properties(gklass);
}

static void
xfw_workspace_dummy_init(XfwWorkspaceDummy *workspace) {
    workspace->priv = xfw_workspace_dummy_get_instance_private(workspace);
}

static void
xfw_workspace_dummy_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    switch (prop_id) {
        case WORKSPACE_PROP_GROUP:
        case WORKSPACE_PROP_ID:
        case WORKSPACE_PROP_NAME:
        case WORKSPACE_PROP_CAPABILITIES:
        case WORKSPACE_PROP_STATE:
        case WORKSPACE_PROP_NUMBER:
        case WORKSPACE_PROP_GEOMETRY:
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_dummy_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWorkspace *workspace = XFW_WORKSPACE(obj);

    switch (prop_id) {
        case WORKSPACE_PROP_GROUP:
            g_value_set_object(value, XFW_WORKSPACE_DUMMY(workspace)->priv->group);
            break;

        case WORKSPACE_PROP_ID:
            g_value_set_string(value, xfw_workspace_dummy_get_id(workspace));
            break;

        case WORKSPACE_PROP_NAME:
            g_value_set_string(value, xfw_workspace_dummy_get_name(workspace));
            break;

        case WORKSPACE_PROP_CAPABILITIES:
            g_value_set_flags(value, xfw_workspace_dummy_get_capabilities(workspace));
            break;

        case WORKSPACE_PROP_STATE:
            g_value_set_flags(value, xfw_workspace_dummy_get_state(workspace));
            break;

        case WORKSPACE_PROP_NUMBER:
            g_value_set_uint(value, xfw_workspace_dummy_get_number(workspace));
            break;

        case WORKSPACE_PROP_GEOMETRY:
            g_value_take_boxed(value, g_memdup2(xfw_workspace_dummy_get_geometry(workspace), sizeof(GdkRectangle)));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_dummy_workspace_init(XfwWorkspaceIface *iface) {
    iface->get_id = xfw_workspace_dummy_get_id;
    iface->get_name = xfw_workspace_dummy_get_name;
    iface->get_capabilities = xfw_workspace_dummy_get_capabilities;
    iface->get_state = xfw_workspace_dummy_get_state;
    iface->get_number = xfw_workspace_dummy_get_number;
    iface->get_workspace_group = xfw_workspace_dummy_get_workspace_group;
    iface->get_layout_row = xfw_workspace_dummy_get_layout_row;
    iface->get_layout_column = xfw_workspace_dummy_get_layout_column;
    iface->get_neighbor = xfw_workspace_dummy_get_neighbor;
    iface->get_geometry = xfw_workspace_dummy_get_geometry;
    iface->activate = xfw_workspace_dummy_activate;
    iface->remove = xfw_workspace_dummy_remove;
    iface->assign_to_workspace_group = xfw_workspace_dummy_assign_to_workspace_group;
}

static const gchar *
xfw_workspace_dummy_get_id(XfwWorkspace *workspace) {
    return "0";
}

static const gchar *
xfw_workspace_dummy_get_name(XfwWorkspace *workspace) {
    return _("Workspace 0");
}

static XfwWorkspaceCapabilities
xfw_workspace_dummy_get_capabilities(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_CAPABILITIES_ACTIVATE;
}

static XfwWorkspaceState
xfw_workspace_dummy_get_state(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_STATE_ACTIVE;
}

static guint
xfw_workspace_dummy_get_number(XfwWorkspace *workspace) {
    return 0;
}

static XfwWorkspaceGroup *
xfw_workspace_dummy_get_workspace_group(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_DUMMY(workspace)->priv->group;
}

gint
xfw_workspace_dummy_get_layout_row(XfwWorkspace *workspace) {
    return 0;
}

gint
xfw_workspace_dummy_get_layout_column(XfwWorkspace *workspace) {
    return 0;
}

XfwWorkspace *
xfw_workspace_dummy_get_neighbor(XfwWorkspace *workspace, XfwDirection direction) {
    return NULL;
}

static GdkRectangle *
xfw_workspace_dummy_get_geometry(XfwWorkspace *workspace) {
    return &XFW_WORKSPACE_DUMMY(workspace)->priv->geometry;
}

static gboolean
xfw_workspace_dummy_activate(XfwWorkspace *workspace, GError **error) {
    return TRUE;
}

static gboolean
xfw_workspace_dummy_remove(XfwWorkspace *workspace, GError **error) {
    if (error != NULL) {
        *error = g_error_new_literal(XFW_ERROR, 0, "Cannot remove workspace as it is the only one left");
    }
    return FALSE;
}

static gboolean
xfw_workspace_dummy_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error) {
    return TRUE;
}

void
_xfw_workspace_dummy_set_workspace_group(XfwWorkspaceDummy *workspace, XfwWorkspaceGroup *group) {
    if (group != workspace->priv->group) {
        XfwWorkspaceGroup *previous_group = workspace->priv->group;
        workspace->priv->group = group;
        g_signal_emit_by_name(workspace, "group-changed", previous_group);
    }
}
