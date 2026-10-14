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

#include <libwnck/libwnck.h>

#include "libxfce4windowing-private.h"
#include "xfw-util.h"
#include "xfw-workspace-manager-x11.h"
#include "xfw-workspace-private.h"
#include "xfw-workspace-x11.h"

enum {
    PROP_0,
    PROP_WNCK_WORKSPACE,
};

struct _XfwWorkspaceX11Private {
    gchar *id;
    XfwWorkspaceGroup *group;
    WnckWorkspace *wnck_workspace;
    GdkRectangle geometry;
    gint layout_column;
    gint layout_row;
};

static void xfw_workspace_x11_workspace_init(XfwWorkspaceIface *iface);
static void xfw_workspace_x11_constructed(GObject *obj);
static void xfw_workspace_x11_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_workspace_x11_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_workspace_x11_finalize(GObject *obj);
static const gchar *xfw_workspace_x11_get_id(XfwWorkspace *workspace);
static const gchar *xfw_workspace_x11_get_name(XfwWorkspace *workspace);
static XfwWorkspaceCapabilities xfw_workspace_x11_get_capabilities(XfwWorkspace *workspace);
static XfwWorkspaceState xfw_workspace_x11_get_state(XfwWorkspace *workspace);
static guint xfw_workspace_x11_get_number(XfwWorkspace *workspace);
static XfwWorkspaceGroup *xfw_workspace_x11_get_workspace_group(XfwWorkspace *workspace);
static gint xfw_workspace_x11_get_layout_row(XfwWorkspace *workspace);
static gint xfw_workspace_x11_get_layout_column(XfwWorkspace *workspace);
static XfwWorkspace *xfw_workspace_x11_get_neighbor(XfwWorkspace *workspace, XfwDirection direction);
static GdkRectangle *xfw_workspace_x11_get_geometry(XfwWorkspace *workspace);
static gboolean xfw_workspace_x11_activate(XfwWorkspace *workspace, GError **error);
static gboolean xfw_workspace_x11_remove(XfwWorkspace *workspace, GError **error);
static gboolean xfw_workspace_x11_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error);

static void name_changed(WnckWorkspace *wnck_workspace, XfwWorkspaceX11 *workspace);

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWorkspaceX11, xfw_workspace_x11, G_TYPE_OBJECT,
                              G_ADD_PRIVATE(XfwWorkspaceX11)
                              G_IMPLEMENT_INTERFACE(XFW_TYPE_WORKSPACE,
                                                    xfw_workspace_x11_workspace_init))

static void
xfw_workspace_x11_class_init(XfwWorkspaceX11Class *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);

    gklass->constructed = xfw_workspace_x11_constructed;
    gklass->set_property = xfw_workspace_x11_set_property;
    gklass->get_property = xfw_workspace_x11_get_property;
    gklass->finalize = xfw_workspace_x11_finalize;

    g_object_class_install_property(gklass,
                                    PROP_WNCK_WORKSPACE,
                                    g_param_spec_object("wnck-workspace",
                                                        "wnck-workspace",
                                                        "wnck-workspace",
                                                        WNCK_TYPE_WORKSPACE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
    _xfw_workspace_install_properties(gklass);
}

static void
xfw_workspace_x11_init(XfwWorkspaceX11 *workspace) {
    workspace->priv = xfw_workspace_x11_get_instance_private(workspace);
}

static void
xfw_workspace_x11_constructed(GObject *obj) {
    XfwWorkspaceX11 *workspace = XFW_WORKSPACE_X11(obj);

    g_signal_connect(workspace->priv->wnck_workspace, "name-changed", G_CALLBACK(name_changed), workspace);

    workspace->priv->layout_column = wnck_workspace_get_layout_column(workspace->priv->wnck_workspace);
    workspace->priv->layout_row = wnck_workspace_get_layout_row(workspace->priv->wnck_workspace);

    G_OBJECT_CLASS(xfw_workspace_x11_parent_class)->constructed(obj);
}

static void
xfw_workspace_x11_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWorkspaceX11 *workspace = XFW_WORKSPACE_X11(obj);

    switch (prop_id) {
        case PROP_WNCK_WORKSPACE:
            workspace->priv->wnck_workspace = g_value_dup_object(value);
            break;

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
xfw_workspace_x11_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWorkspace *workspace = XFW_WORKSPACE(obj);

    switch (prop_id) {
        case PROP_WNCK_WORKSPACE:
            g_value_set_object(value, XFW_WORKSPACE_X11(workspace)->priv->wnck_workspace);
            break;

        case WORKSPACE_PROP_GROUP:
            g_value_set_object(value, xfw_workspace_x11_get_workspace_group(workspace));
            break;

        case WORKSPACE_PROP_ID:
            g_value_set_string(value, xfw_workspace_x11_get_id(workspace));
            break;

        case WORKSPACE_PROP_NAME:
            g_value_set_string(value, xfw_workspace_x11_get_name(workspace));
            break;

        case WORKSPACE_PROP_CAPABILITIES:
            g_value_set_flags(value, xfw_workspace_x11_get_capabilities(workspace));
            break;

        case WORKSPACE_PROP_STATE:
            g_value_set_flags(value, xfw_workspace_x11_get_state(workspace));
            break;

        case WORKSPACE_PROP_NUMBER:
            g_value_set_uint(value, xfw_workspace_x11_get_number(workspace));
            break;

        case WORKSPACE_PROP_LAYOUT_ROW:
            g_value_set_int(value, xfw_workspace_x11_get_layout_row(workspace));
            break;

        case WORKSPACE_PROP_LAYOUT_COLUMN:
            g_value_set_int(value, xfw_workspace_x11_get_layout_column(workspace));
            break;

        case WORKSPACE_PROP_GEOMETRY:
            g_value_take_boxed(value, g_memdup2(xfw_workspace_x11_get_geometry(workspace), sizeof(GdkRectangle)));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_x11_finalize(GObject *obj) {
    XfwWorkspaceX11 *workspace = XFW_WORKSPACE_X11(obj);

    g_signal_handlers_disconnect_by_func(workspace->priv->wnck_workspace, name_changed, workspace);

    g_free(workspace->priv->id);

    // to be released last
    g_object_unref(workspace->priv->wnck_workspace);

    G_OBJECT_CLASS(xfw_workspace_x11_parent_class)->finalize(obj);
}

static void
xfw_workspace_x11_workspace_init(XfwWorkspaceIface *iface) {
    iface->get_id = xfw_workspace_x11_get_id;
    iface->get_name = xfw_workspace_x11_get_name;
    iface->get_capabilities = xfw_workspace_x11_get_capabilities;
    iface->get_state = xfw_workspace_x11_get_state;
    iface->get_number = xfw_workspace_x11_get_number;
    iface->get_workspace_group = xfw_workspace_x11_get_workspace_group;
    iface->get_layout_row = xfw_workspace_x11_get_layout_row;
    iface->get_layout_column = xfw_workspace_x11_get_layout_column;
    iface->get_neighbor = xfw_workspace_x11_get_neighbor;
    iface->get_geometry = xfw_workspace_x11_get_geometry;
    iface->activate = xfw_workspace_x11_activate;
    iface->remove = xfw_workspace_x11_remove;
    iface->assign_to_workspace_group = xfw_workspace_x11_assign_to_workspace_group;
}

static const gchar *
xfw_workspace_x11_get_id(XfwWorkspace *workspace) {
    // On X11, specific workspaces are never destroyed, per se; the number of total workspaces can be changed,
    // and making that number smaller means destroying the workspaces at the end of the list.  This means that
    // our IDs -- the workspace index number -- are technically not stable, but in practice, they are, as we'll
    // never have a case where a new workspace is inserted in the middle of exisitng workspaces, or a workspace
    // in the middle is destroyed.
    //
    // When more than one workspace is destroyed at the same time (that is, when the number of workspaces is set
    // to at least 2 less than the currnet number), libwnck will notify us in ascending order of their destruction.
    // Even then, we want to keep the IDs stable: technically, if there are 10 workspaces, and the number is
    // decreased to 5, workspace with ID 5 will get destroyed first, and then the workspace with ID 6 -- for a
    // brief moment, before it's destroyed as well -- should technically become ID 5.  But that would likely break
    // callers when they try to remove data associated when ID 6, 7, 8, and 9 get destroyed, so we will not change
    // the IDs once created.
    XfwWorkspaceX11 *xworkspace = XFW_WORKSPACE_X11(workspace);
    if (xworkspace->priv->id == NULL) {
        xworkspace->priv->id = g_strdup_printf("%u", wnck_workspace_get_number(xworkspace->priv->wnck_workspace));
    }
    return xworkspace->priv->id;
}

static const gchar *
xfw_workspace_x11_get_name(XfwWorkspace *workspace) {
    return wnck_workspace_get_name(XFW_WORKSPACE_X11(workspace)->priv->wnck_workspace);
}

static XfwWorkspaceCapabilities
xfw_workspace_x11_get_capabilities(XfwWorkspace *workspace) {
    XfwWorkspaceX11 *xworkspace = XFW_WORKSPACE_X11(workspace);
    XfwWorkspaceCapabilities capabilities = XFW_WORKSPACE_CAPABILITIES_ACTIVATE;
    gint n_workspaces = wnck_screen_get_workspace_count(wnck_workspace_get_screen(xworkspace->priv->wnck_workspace));

    if (n_workspaces == wnck_workspace_get_number(xworkspace->priv->wnck_workspace) + 1) {
        // This is perhaps not universal, but the Wnck implementation (which xfdesktop/xfce4-panel/xfwm4 conforms
        // to) only really lets you remove the last workspace.
        capabilities |= XFW_WORKSPACE_CAPABILITIES_REMOVE;
    }

    return capabilities;
}

static XfwWorkspaceState
xfw_workspace_x11_get_state(XfwWorkspace *workspace) {
    XfwWorkspaceX11Private *priv = XFW_WORKSPACE_X11(workspace)->priv;
    XfwWorkspaceState state = XFW_WORKSPACE_STATE_NONE;
    if (wnck_screen_get_active_workspace(wnck_workspace_get_screen(priv->wnck_workspace)) == priv->wnck_workspace) {
        state |= XFW_WORKSPACE_STATE_ACTIVE;
    }
    if (wnck_workspace_is_virtual(priv->wnck_workspace)) {
        state |= XFW_WORKSPACE_STATE_VIRTUAL;
    }
    return state;
}

static guint
xfw_workspace_x11_get_number(XfwWorkspace *workspace) {
    return wnck_workspace_get_number(XFW_WORKSPACE_X11(workspace)->priv->wnck_workspace);
}

static XfwWorkspaceGroup *
xfw_workspace_x11_get_workspace_group(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_X11(workspace)->priv->group;
}

static gint
xfw_workspace_x11_get_layout_row(XfwWorkspace *workspace) {
    XfwWorkspaceX11Private *priv = XFW_WORKSPACE_X11(workspace)->priv;
    priv->layout_row = wnck_workspace_get_layout_row(priv->wnck_workspace);
    return priv->layout_row;
}

static gint
xfw_workspace_x11_get_layout_column(XfwWorkspace *workspace) {
    XfwWorkspaceX11Private *priv = XFW_WORKSPACE_X11(workspace)->priv;
    priv->layout_column = wnck_workspace_get_layout_column(priv->wnck_workspace);
    return priv->layout_column;
}

static XfwWorkspace *
xfw_workspace_x11_get_neighbor(XfwWorkspace *workspace, XfwDirection direction) {
    XfwWorkspaceX11 *xworkspace = XFW_WORKSPACE_X11(workspace);
    WnckMotionDirection wnck_direction;
    WnckWorkspace *wnck_workspace;

    switch (direction) {
        case XFW_DIRECTION_UP:
            wnck_direction = WNCK_MOTION_UP;
            break;
        case XFW_DIRECTION_DOWN:
            wnck_direction = WNCK_MOTION_DOWN;
            break;
        case XFW_DIRECTION_LEFT:
            wnck_direction = WNCK_MOTION_LEFT;
            break;
        case XFW_DIRECTION_RIGHT:
            wnck_direction = WNCK_MOTION_RIGHT;
            break;
        default:
            g_critical("Invalid XfwDirection %d", direction);
            return NULL;
    }

    wnck_workspace = wnck_workspace_get_neighbor(xworkspace->priv->wnck_workspace, wnck_direction);
    if (wnck_workspace != NULL) {
        XfwWorkspaceManager *manager = xfw_workspace_group_get_workspace_manager(xworkspace->priv->group);
        return _xfw_workspace_manager_x11_workspace_for_wnck_workspace(XFW_WORKSPACE_MANAGER_X11(manager), wnck_workspace);
    } else {
        return NULL;
    }
}

static GdkRectangle *
xfw_workspace_x11_get_geometry(XfwWorkspace *workspace) {
    XfwWorkspaceX11Private *priv = XFW_WORKSPACE_X11(workspace)->priv;
    gboolean virtual = wnck_workspace_is_virtual(priv->wnck_workspace);
    priv->geometry.x = virtual ? wnck_workspace_get_viewport_x(priv->wnck_workspace) : 0;
    priv->geometry.y = virtual ? wnck_workspace_get_viewport_y(priv->wnck_workspace) : 0;
    priv->geometry.width = wnck_workspace_get_width(priv->wnck_workspace);
    priv->geometry.height = wnck_workspace_get_height(priv->wnck_workspace);
    return &priv->geometry;
}

static gboolean
xfw_workspace_x11_activate(XfwWorkspace *workspace, GError **error) {
    XfwWorkspaceX11Private *priv = XFW_WORKSPACE_X11(workspace)->priv;
    wnck_workspace_activate(priv->wnck_workspace, g_get_monotonic_time() / 1000);
    return TRUE;
}

static gboolean
xfw_workspace_x11_remove(XfwWorkspace *workspace, GError **error) {
    XfwWorkspaceX11Private *priv = XFW_WORKSPACE_X11(workspace)->priv;
    WnckScreen *wnck_screen = wnck_workspace_get_screen(priv->wnck_workspace);
    gint count = wnck_screen_get_workspace_count(wnck_screen);
    if (count > 1) {
        wnck_screen_change_workspace_count(wnck_screen, count - 1);
        return TRUE;
    } else {
        if (error != NULL) {
            *error = g_error_new_literal(XFW_ERROR, 0, "Cannot remove workspace as it is the only one left");
        }
        return FALSE;
    }
}

static gboolean
xfw_workspace_x11_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error) {
    // On X11 there is only ever one group, and workspaces are always
    // members of it, so this always succeeds.
    return TRUE;
}

static void
name_changed(WnckWorkspace *wnck_workspace, XfwWorkspaceX11 *workspace) {
    g_object_notify(G_OBJECT(workspace), "name");
    g_signal_emit_by_name(workspace, "name-changed");
}

WnckWorkspace *
_xfw_workspace_x11_get_wnck_workspace(XfwWorkspaceX11 *workspace) {
    return workspace->priv->wnck_workspace;
}

void
_xfw_workspace_x11_set_workspace_group(XfwWorkspaceX11 *workspace, XfwWorkspaceGroup *group) {
    if (group != workspace->priv->group) {
        XfwWorkspaceGroup *previous_group = workspace->priv->group;
        workspace->priv->group = group;
        g_signal_emit_by_name(workspace, "group-changed", previous_group);
    }
}

void
_xfw_workspace_x11_check_properties_changed(XfwWorkspaceX11 *workspace) {
    GdkRectangle old_geometry = workspace->priv->geometry;
    GdkRectangle *new_geometry = xfw_workspace_x11_get_geometry(XFW_WORKSPACE(workspace));

    gint old_layout_row = workspace->priv->layout_row;
    gint new_layout_row = xfw_workspace_x11_get_layout_row(XFW_WORKSPACE(workspace));

    gint old_layout_column = workspace->priv->layout_column;
    gint new_layout_column = xfw_workspace_x11_get_layout_column(XFW_WORKSPACE(workspace));

    if (!gdk_rectangle_equal(&old_geometry, new_geometry)) {
        g_object_notify(G_OBJECT(workspace), "geometry");
    }

    if (old_layout_row != new_layout_row) {
        g_object_notify(G_OBJECT(workspace), "layout-row");
    }

    if (old_layout_column != new_layout_column) {
        g_object_notify(G_OBJECT(workspace), "layout-column");
    }
}
