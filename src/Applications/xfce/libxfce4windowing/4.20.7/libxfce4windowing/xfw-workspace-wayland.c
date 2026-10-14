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

#include <limits.h>

#include "protocols/ext-workspace-v1-client.h"

#include "libxfce4windowing-private.h"
#include "xfw-monitor.h"
#include "xfw-util.h"
#include "xfw-workspace-group-wayland.h"
#include "xfw-workspace-group.h"
#include "xfw-workspace-private.h"
#include "xfw-workspace-wayland.h"

struct _XfwWorkspaceWaylandPrivate {
    XfwWorkspaceGroup *group;
    struct ext_workspace_handle_v1 *handle;
    struct ext_workspace_manager_v1 *manager_handle;
    gchar *id;
    gchar *name;
    XfwWorkspaceCapabilities capabilities;
    XfwWorkspaceState state;
    guint number;
    gint row;
    gint column;
    GdkRectangle geometry;
};

enum {
    SIGNAL_DESTROYED,

    N_SIGNALS,
};

enum {
    PROP_0,
    PROP_HANDLE,
};

static guint workspace_signals[N_SIGNALS] = { 0 };

static void xfw_workspace_wayland_workspace_init(XfwWorkspaceIface *iface);
static void xfw_workspace_wayland_constructed(GObject *obj);
static void xfw_workspace_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_workspace_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_workspace_wayland_finalize(GObject *obj);
static const gchar *xfw_workspace_wayland_get_id(XfwWorkspace *workspace);
static const gchar *xfw_workspace_wayland_get_name(XfwWorkspace *workspace);
static XfwWorkspaceCapabilities xfw_workspace_wayland_get_capabilities(XfwWorkspace *workspace);
static XfwWorkspaceState xfw_workspace_wayland_get_state(XfwWorkspace *workspace);
static guint xfw_workspace_wayland_get_number(XfwWorkspace *workspace);
static XfwWorkspaceGroup *xfw_workspace_wayland_get_workspace_group(XfwWorkspace *workspace);
static gint xfw_workspace_wayland_get_layout_row(XfwWorkspace *workspace);
static gint xfw_workspace_wayland_get_layout_column(XfwWorkspace *workspace);
static XfwWorkspace *xfw_workspace_wayland_get_neighbor(XfwWorkspace *workspace, XfwDirection direction);
static GdkRectangle *xfw_workspace_wayland_get_geometry(XfwWorkspace *workspace);
static gboolean xfw_workspace_wayland_activate(XfwWorkspace *workspace, GError **error);
static gboolean xfw_workspace_wayland_remove(XfwWorkspace *workspace, GError **error);
static gboolean xfw_workspace_wayland_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error);

static void workspace_id(void *data, struct ext_workspace_handle_v1 *workspace, const char *id);
static void workspace_name(void *data, struct ext_workspace_handle_v1 *workspace, const char *name);
static void workspace_coordinates(void *data, struct ext_workspace_handle_v1 *workspace, struct wl_array *coordinates);
static void workspace_state(void *data, struct ext_workspace_handle_v1 *workspace, uint32_t state);
static void workspace_capabilities(void *data, struct ext_workspace_handle_v1 *workspace, uint32_t capabilities);
static void workspace_removed(void *data, struct ext_workspace_handle_v1 *workspace);

static const struct ext_workspace_handle_v1_listener workspace_listener = {
    .id = workspace_id,
    .name = workspace_name,
    .coordinates = workspace_coordinates,
    .state = workspace_state,
    .capabilities = workspace_capabilities,
    .removed = workspace_removed,
};

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWorkspaceWayland, xfw_workspace_wayland, G_TYPE_OBJECT,
                              G_ADD_PRIVATE(XfwWorkspaceWayland)
                              G_IMPLEMENT_INTERFACE(XFW_TYPE_WORKSPACE,
                                                    xfw_workspace_wayland_workspace_init))

static void
xfw_workspace_wayland_class_init(XfwWorkspaceWaylandClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);

    gklass->constructed = xfw_workspace_wayland_constructed;
    gklass->set_property = xfw_workspace_wayland_set_property;
    gklass->get_property = xfw_workspace_wayland_get_property;
    gklass->finalize = xfw_workspace_wayland_finalize;

    workspace_signals[SIGNAL_DESTROYED] = g_signal_new("destroyed",
                                                       XFW_TYPE_WORKSPACE_WAYLAND,
                                                       G_SIGNAL_RUN_LAST,
                                                       0, NULL, NULL,
                                                       g_cclosure_marshal_VOID__VOID,
                                                       G_TYPE_NONE, 0);

    g_object_class_install_property(gklass,
                                    PROP_HANDLE,
                                    g_param_spec_pointer("handle",
                                                         "handle",
                                                         "handle",
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
    _xfw_workspace_install_properties(gklass);
}

static void
xfw_workspace_wayland_init(XfwWorkspaceWayland *workspace) {
    workspace->priv = xfw_workspace_wayland_get_instance_private(workspace);
    workspace->priv->row = -1;
    workspace->priv->column = -1;
}

static void
xfw_workspace_wayland_constructed(GObject *obj) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(obj);
    ext_workspace_handle_v1_add_listener(workspace->priv->handle, &workspace_listener, workspace);

    G_OBJECT_CLASS(xfw_workspace_wayland_parent_class)->constructed(obj);
}

static void
xfw_workspace_wayland_workspace_init(XfwWorkspaceIface *iface) {
    iface->get_id = xfw_workspace_wayland_get_id;
    iface->get_name = xfw_workspace_wayland_get_name;
    iface->get_capabilities = xfw_workspace_wayland_get_capabilities;
    iface->get_state = xfw_workspace_wayland_get_state;
    iface->get_number = xfw_workspace_wayland_get_number;
    iface->get_workspace_group = xfw_workspace_wayland_get_workspace_group;
    iface->get_layout_row = xfw_workspace_wayland_get_layout_row;
    iface->get_layout_column = xfw_workspace_wayland_get_layout_column;
    iface->get_neighbor = xfw_workspace_wayland_get_neighbor;
    iface->get_geometry = xfw_workspace_wayland_get_geometry;
    iface->activate = xfw_workspace_wayland_activate;
    iface->remove = xfw_workspace_wayland_remove;
    iface->assign_to_workspace_group = xfw_workspace_wayland_assign_to_workspace_group;
}

static void
xfw_workspace_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(obj);
    switch (prop_id) {
        case PROP_HANDLE:
            workspace->priv->handle = g_value_get_pointer(value);
            break;

        case WORKSPACE_PROP_NAME:
            g_free(workspace->priv->name);
            workspace->priv->name = g_value_dup_string(value);
            break;
        case WORKSPACE_PROP_GROUP:
        case WORKSPACE_PROP_ID:
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
xfw_workspace_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(obj);
    switch (prop_id) {
        case PROP_HANDLE:
            g_value_set_pointer(value, workspace->priv->handle);
            break;

        case WORKSPACE_PROP_GROUP:
            g_value_set_object(value, workspace->priv->group);
            break;

        case WORKSPACE_PROP_ID:
            g_value_set_string(value, workspace->priv->id);
            break;

        case WORKSPACE_PROP_NAME:
            g_value_set_string(value, workspace->priv->name);
            break;

        case WORKSPACE_PROP_CAPABILITIES:
            g_value_set_flags(value, workspace->priv->capabilities);
            break;

        case WORKSPACE_PROP_STATE:
            g_value_set_flags(value, workspace->priv->state);
            break;

        case WORKSPACE_PROP_LAYOUT_ROW:
            g_value_set_int(value, workspace->priv->row);
            break;

        case WORKSPACE_PROP_LAYOUT_COLUMN:
            g_value_set_int(value, workspace->priv->column);
            break;

        case WORKSPACE_PROP_GEOMETRY:
            g_value_take_boxed(value, g_memdup2(&workspace->priv->geometry, sizeof(GdkRectangle)));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_wayland_finalize(GObject *obj) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(obj);

    if (workspace->priv->group != NULL) {
        g_signal_handlers_disconnect_by_data(workspace->priv->group, workspace);
    }

    ext_workspace_handle_v1_destroy(workspace->priv->handle);
    g_free(workspace->priv->id);
    g_free(workspace->priv->name);

    G_OBJECT_CLASS(xfw_workspace_wayland_parent_class)->finalize(obj);
}

static const gchar *
xfw_workspace_wayland_get_id(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_WAYLAND(workspace)->priv->id;
}

static const gchar *
xfw_workspace_wayland_get_name(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_WAYLAND(workspace)->priv->name;
}

static XfwWorkspaceCapabilities
xfw_workspace_wayland_get_capabilities(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_WAYLAND(workspace)->priv->capabilities;
}

static XfwWorkspaceState
xfw_workspace_wayland_get_state(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_WAYLAND(workspace)->priv->state;
}

static guint
xfw_workspace_wayland_get_number(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_WAYLAND(workspace)->priv->number;
}

static XfwWorkspaceGroup *
xfw_workspace_wayland_get_workspace_group(XfwWorkspace *workspace) {
    return XFW_WORKSPACE_WAYLAND(workspace)->priv->group;
}

static gint
xfw_workspace_wayland_get_layout_row(XfwWorkspace *workspace) {
    XfwWorkspaceWayland *wworkspace = XFW_WORKSPACE_WAYLAND(workspace);
    return wworkspace->priv->row >= 0 ? wworkspace->priv->row : 0;
}

static gint
xfw_workspace_wayland_get_layout_column(XfwWorkspace *workspace) {
    XfwWorkspaceWayland *wworkspace = XFW_WORKSPACE_WAYLAND(workspace);
    return wworkspace->priv->column >= 0 ? wworkspace->priv->column : (gint)wworkspace->priv->number;
}

static XfwWorkspace *
xfw_workspace_wayland_get_neighbor(XfwWorkspace *workspace, XfwDirection direction) {
    XfwWorkspaceWayland *wworkspace = XFW_WORKSPACE_WAYLAND(workspace);

    switch (direction) {
        case XFW_DIRECTION_UP:
        case XFW_DIRECTION_DOWN:
            return NULL;

        case XFW_DIRECTION_LEFT: {
            gint num = xfw_workspace_wayland_get_layout_column(workspace);
            if (wworkspace->priv->group == NULL || num < 1) {
                return NULL;
            } else {
                GList *workspaces = xfw_workspace_group_list_workspaces(wworkspace->priv->group);
                return XFW_WORKSPACE(g_list_nth_data(workspaces, num - 1));
            }
        }

        case XFW_DIRECTION_RIGHT: {
            if (wworkspace->priv->group == NULL) {
                return NULL;
            } else {
                gint num = xfw_workspace_wayland_get_layout_column(workspace);
                GList *workspaces = xfw_workspace_group_list_workspaces(wworkspace->priv->group);
                return XFW_WORKSPACE(g_list_nth_data(workspaces, num + 1));
            }
        }
    }

    g_critical("Invalid XfwDirection %d", direction);
    return NULL;
}

static GdkRectangle *
xfw_workspace_wayland_get_geometry(XfwWorkspace *workspace) {
    return &XFW_WORKSPACE_WAYLAND(workspace)->priv->geometry;
}

static gboolean
xfw_workspace_wayland_activate(XfwWorkspace *workspace, GError **error) {
    XfwWorkspaceWayland *wworkspace = XFW_WORKSPACE_WAYLAND(workspace);

    if ((wworkspace->priv->capabilities & XFW_WORKSPACE_CAPABILITIES_ACTIVATE) != 0) {
        ext_workspace_handle_v1_activate(XFW_WORKSPACE_WAYLAND(workspace)->priv->handle);
        ext_workspace_manager_v1_commit(wworkspace->priv->manager_handle);
        return TRUE;
    } else {
        if (error != NULL) {
            *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This workspace does not support activation");
        }
        return FALSE;
    }
}

static gboolean
xfw_workspace_wayland_remove(XfwWorkspace *workspace, GError **error) {
    XfwWorkspaceWayland *wworkspace = XFW_WORKSPACE_WAYLAND(workspace);

    if ((wworkspace->priv->capabilities & XFW_WORKSPACE_CAPABILITIES_REMOVE) != 0) {
        ext_workspace_handle_v1_remove(XFW_WORKSPACE_WAYLAND(workspace)->priv->handle);
        ext_workspace_manager_v1_commit(wworkspace->priv->manager_handle);
        return TRUE;
    } else {
        if (error != NULL) {
            *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This workspace does not support removal");
        }
        return FALSE;
    }
}

static gboolean
xfw_workspace_wayland_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error) {
    XfwWorkspaceWayland *wworkspace = XFW_WORKSPACE_WAYLAND(workspace);

    if ((wworkspace->priv->capabilities & XFW_WORKSPACE_CAPABILITIES_ASSIGN) != 0) {
        ext_workspace_handle_v1_assign(XFW_WORKSPACE_WAYLAND(workspace)->priv->handle,
                                       _xfw_workspace_group_wayland_get_handle(XFW_WORKSPACE_GROUP_WAYLAND(group)));
        ext_workspace_manager_v1_commit(wworkspace->priv->manager_handle);
        return TRUE;
    } else {
        if (error != NULL) {
            *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This workspace does not support group assignment");
        }
        return FALSE;
    }
}

static void
workspace_id(void *data, struct ext_workspace_handle_v1 *wl_workspace, const char *id) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(data);
    g_free(workspace->priv->id);
    workspace->priv->id = g_strdup(id);
    g_object_notify(G_OBJECT(workspace), "id");
}

static void
workspace_name(void *data, struct ext_workspace_handle_v1 *wl_workspace, const char *name) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(data);
    g_object_set(workspace, "name", name, NULL);
    g_signal_emit_by_name(workspace, "name-changed");
}

static void
workspace_coordinates(void *data, struct ext_workspace_handle_v1 *wl_workspace, struct wl_array *coordinates) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(data);

    uint32_t *array_start = coordinates->data;

    g_object_freeze_notify(G_OBJECT(workspace));

    if (coordinates->size >= 1 && (int32_t)array_start[0] != workspace->priv->column) {
        workspace->priv->column = array_start[0];
        g_object_notify(G_OBJECT(workspace), "layout-column");
    }
    if (coordinates->size >= 2 && (int32_t)array_start[1] != workspace->priv->row) {
        workspace->priv->row = array_start[1];
        g_object_notify(G_OBJECT(workspace), "layout-row");
    }

    g_object_thaw_notify(G_OBJECT(workspace));
}

static const struct {
    enum ext_workspace_handle_v1_state wl_state;
    XfwWorkspaceState state_bit;
} state_converters[] = {
    { EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE, XFW_WORKSPACE_STATE_ACTIVE },
    { EXT_WORKSPACE_HANDLE_V1_STATE_URGENT, XFW_WORKSPACE_STATE_URGENT },
    { EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN, XFW_WORKSPACE_STATE_HIDDEN },
};

static void
workspace_state(void *data, struct ext_workspace_handle_v1 *wl_workspace, uint32_t wl_state) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(data);
    XfwWorkspaceState old_state = workspace->priv->state;
    XfwWorkspaceState changed_mask;
    XfwWorkspaceState new_state = XFW_WORKSPACE_STATE_NONE;

    for (gsize i = 0; i < G_N_ELEMENTS(state_converters); ++i) {
        if ((state_converters[i].wl_state & wl_state) != 0) {
            new_state |= state_converters[i].state_bit;
            break;
        }
    }

    workspace->priv->state = new_state;
    changed_mask = old_state ^ new_state;
    g_object_notify(G_OBJECT(workspace), "state");
    g_signal_emit_by_name(workspace, "state-changed", changed_mask, new_state);
    if (workspace->priv->group != NULL && (changed_mask & XFW_WORKSPACE_STATE_ACTIVE) != 0) {
        if ((new_state & XFW_WORKSPACE_STATE_ACTIVE) != 0) {
            _xfw_workspace_group_wayland_set_active_workspace(XFW_WORKSPACE_GROUP_WAYLAND(workspace->priv->group), XFW_WORKSPACE(workspace));
        } else {
            XfwWorkspace *current_active_workspace = xfw_workspace_group_get_active_workspace(workspace->priv->group);
            // If what's set as the current active workspace is *not* this workspace, then the newly-active
            // workspace has already been set, so only unset it if what's currently active is this workspace.
            if (current_active_workspace == XFW_WORKSPACE(workspace)) {
                _xfw_workspace_group_wayland_set_active_workspace(XFW_WORKSPACE_GROUP_WAYLAND(workspace->priv->group), NULL);
            }
        }
    }
}

static const struct {
    enum ext_workspace_handle_v1_workspace_capabilities wl_capability;
    XfwWorkspaceCapabilities capability_bit;
} capabilities_converters[] = {
    { EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE, XFW_WORKSPACE_CAPABILITIES_ACTIVATE },
    { EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE, XFW_WORKSPACE_CAPABILITIES_DEACTIVATE },
    { EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_REMOVE, XFW_WORKSPACE_CAPABILITIES_REMOVE },
    { EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN, XFW_WORKSPACE_CAPABILITIES_ASSIGN },
};

static void
workspace_capabilities(void *data, struct ext_workspace_handle_v1 *wl_workspace, uint32_t wl_capabilities) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(data);
    XfwWorkspaceCapabilities old_capabilities = workspace->priv->capabilities;
    XfwWorkspaceCapabilities changed_mask;
    XfwWorkspaceCapabilities new_capabilities = XFW_WORKSPACE_CAPABILITIES_NONE;

    for (gsize i = 0; i < G_N_ELEMENTS(capabilities_converters); ++i) {
        if ((capabilities_converters[i].wl_capability & wl_capabilities) != 0) {
            new_capabilities |= capabilities_converters[i].capability_bit;
            break;
        }
    }

    workspace->priv->capabilities = new_capabilities;
    changed_mask = old_capabilities ^ new_capabilities;
    g_object_notify(G_OBJECT(workspace), "capabilities");
    g_signal_emit_by_name(workspace, "capabilities-changed", changed_mask, new_capabilities);
}

static void
workspace_removed(void *data, struct ext_workspace_handle_v1 *wl_workspace) {
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(data);
    g_signal_emit(workspace, workspace_signals[SIGNAL_DESTROYED], 0);
}

static void
group_monitors_changed(XfwWorkspaceGroup *group, XfwWorkspaceWayland *workspace) {
    GdkRectangle new_geom = { 0 };

    GList *monitors = xfw_workspace_group_get_monitors(group);
    if (monitors != NULL) {
        XfwMonitor *monitor = XFW_MONITOR(monitors->data);
        xfw_monitor_get_physical_geometry(monitor, &new_geom);
        monitors = monitors->next;
    }

    for (GList *l = monitors; l != NULL; l = l->next) {
        XfwMonitor *monitor = XFW_MONITOR(l->data);
        GdkRectangle geom;
        xfw_monitor_get_physical_geometry(monitor, &geom);
        gdk_rectangle_union(&new_geom, &geom, &new_geom);
    }

    if (!gdk_rectangle_equal(&new_geom, &workspace->priv->geometry)) {
        workspace->priv->geometry = new_geom;
        g_object_notify(G_OBJECT(workspace), "geometry");
    }
}

struct ext_workspace_handle_v1 *
_xfw_workspace_wayland_get_handle(XfwWorkspaceWayland *workspace) {
    return workspace->priv->handle;
}

void
_xfw_workspace_wayland_set_number(XfwWorkspaceWayland *workspace, guint number) {
    if (number != workspace->priv->number) {
        workspace->priv->number = number;
        g_object_notify(G_OBJECT(workspace), "number");
    }
}

void
_xfw_workspace_wayland_set_workspace_group(XfwWorkspaceWayland *workspace, XfwWorkspaceGroup *group) {
    if (group != workspace->priv->group) {
        if (workspace->priv->group != NULL) {
            g_signal_handlers_disconnect_by_data(workspace->priv->group, workspace);
        }

        XfwWorkspaceGroup *previous_group = workspace->priv->group;
        workspace->priv->group = group;

        if (group != NULL) {
            g_signal_connect(group, "monitors-changed", G_CALLBACK(group_monitors_changed), workspace);
            group_monitors_changed(group, workspace);

            if ((workspace->priv->state & XFW_WORKSPACE_STATE_ACTIVE) != 0) {
                // ensure active workspace is initialized, since workspace
                // state can be sent before workspace enters the group
                _xfw_workspace_group_wayland_set_active_workspace(XFW_WORKSPACE_GROUP_WAYLAND(group), XFW_WORKSPACE(workspace));
            }
        }

        g_signal_emit_by_name(workspace, "group-changed", previous_group);
    }
}

void
_xfw_workspace_wayland_set_workspace_manager_handle(XfwWorkspaceWayland *workspace, struct ext_workspace_manager_v1 *manager_handle) {
    workspace->priv->manager_handle = manager_handle;
}
