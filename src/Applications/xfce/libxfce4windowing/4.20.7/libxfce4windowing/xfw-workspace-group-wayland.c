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
#include <limits.h>

#include "protocols/ext-workspace-v1-client.h"

#include "libxfce4windowing-private.h"
#include "xfw-monitor-wayland.h"
#include "xfw-screen.h"
#include "xfw-util.h"
#include "xfw-workspace-group-private.h"
#include "xfw-workspace-group-wayland.h"
#include "xfw-workspace-manager.h"
#include "xfw-workspace-wayland.h"
#include "xfw-workspace.h"

enum {
    SIGNAL_DESTROYED,

    N_SIGNALS,
};

enum {
    PROP_0,
    PROP_HANDLE,
};

struct _XfwWorkspaceGroupWaylandPrivate {
    XfwScreen *screen;
    XfwWorkspaceManager *workspace_manager;
    struct ext_workspace_group_handle_v1 *handle;
    XfwWorkspaceGroupCapabilities capabilities;
    GList *workspaces;
    XfwWorkspace *active_workspace;
    GList *monitors;
    GHashTable *advertised_outputs;
};

static guint group_signals[N_SIGNALS] = { 0 };

static void xfw_workspace_group_wayland_workspace_group_init(XfwWorkspaceGroupIface *iface);
static void xfw_workspace_group_wayland_constructed(GObject *obj);
static void xfw_workspace_group_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_workspace_group_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_workspace_group_wayland_finalize(GObject *obj);
static XfwWorkspaceGroupCapabilities xfw_workspace_group_wayland_get_capabilities(XfwWorkspaceGroup *group);
static guint xfw_workspace_group_wayland_get_workspace_count(XfwWorkspaceGroup *group);
static GList *xfw_workspace_group_wayland_list_workspaces(XfwWorkspaceGroup *group);
static XfwWorkspace *xfw_workspace_group_wayland_get_active_workspace(XfwWorkspaceGroup *group);
static GList *xfw_workspace_group_wayland_get_monitors(XfwWorkspaceGroup *group);
static XfwWorkspaceManager *xfw_workspace_group_wayland_get_workspace_manager(XfwWorkspaceGroup *group);
static gboolean xfw_workspace_group_wayland_create_workspace(XfwWorkspaceGroup *group, const gchar *name, GError **error);
static gboolean xfw_workspace_group_wayland_move_viewport(XfwWorkspaceGroup *group, gint x, gint y, GError **error);
static gboolean xfw_workspace_group_wayland_set_layout(XfwWorkspaceGroup *group, gint rows, gint columns, GError **error);

static void group_capabilities(void *data, struct ext_workspace_group_handle_v1 *group, uint32_t capabilities);
static void group_output_enter(void *data, struct ext_workspace_group_handle_v1 *group, struct wl_output *output);
static void group_output_leave(void *data, struct ext_workspace_group_handle_v1 *group, struct wl_output *output);
static void group_workspace_enter(void *data, struct ext_workspace_group_handle_v1 *group, struct ext_workspace_handle_v1 *workspace);
static void group_workspace_leave(void *data, struct ext_workspace_group_handle_v1 *group, struct ext_workspace_handle_v1 *workspace);
static void group_removed(void *data, struct ext_workspace_group_handle_v1 *group);

static void screen_monitor_added(XfwScreen *screen, XfwMonitor *monitor, XfwWorkspaceGroupWayland *group);
static void screen_monitor_removed(XfwScreen *screen, XfwMonitor *monitor, XfwWorkspaceGroupWayland *group);

static const struct ext_workspace_group_handle_v1_listener group_listener = {
    .capabilities = group_capabilities,
    .output_enter = group_output_enter,
    .output_leave = group_output_leave,
    .workspace_enter = group_workspace_enter,
    .workspace_leave = group_workspace_leave,
    .removed = group_removed,
};

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWorkspaceGroupWayland, xfw_workspace_group_wayland, G_TYPE_OBJECT,
                              G_ADD_PRIVATE(XfwWorkspaceGroupWayland)
                              G_IMPLEMENT_INTERFACE(XFW_TYPE_WORKSPACE_GROUP,
                                                    xfw_workspace_group_wayland_workspace_group_init))

static void
xfw_workspace_group_wayland_class_init(XfwWorkspaceGroupWaylandClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);

    gklass->constructed = xfw_workspace_group_wayland_constructed;
    gklass->set_property = xfw_workspace_group_wayland_set_property;
    gklass->get_property = xfw_workspace_group_wayland_get_property;
    gklass->finalize = xfw_workspace_group_wayland_finalize;

    group_signals[SIGNAL_DESTROYED] = g_signal_new("destroyed",
                                                   XFW_TYPE_WORKSPACE_GROUP_WAYLAND,
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
    _xfw_workspace_group_install_properties(gklass);
}

static void
xfw_workspace_group_wayland_init(XfwWorkspaceGroupWayland *group) {
    group->priv = xfw_workspace_group_wayland_get_instance_private(group);
    group->priv->advertised_outputs = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void
xfw_workspace_group_wayland_constructed(GObject *obj) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(obj);
    ext_workspace_group_handle_v1_add_listener(group->priv->handle, &group_listener, group);
    g_signal_connect(group->priv->screen, "monitor-added", G_CALLBACK(screen_monitor_added), group);
    g_signal_connect(group->priv->screen, "monitor-removed", G_CALLBACK(screen_monitor_removed), group);

    G_OBJECT_CLASS(xfw_workspace_group_wayland_parent_class)->constructed(obj);
}

static void
xfw_workspace_group_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(obj);

    switch (prop_id) {
        case PROP_HANDLE:
            group->priv->handle = g_value_get_pointer(value);
            break;

        case WORKSPACE_GROUP_PROP_SCREEN:
            group->priv->screen = g_value_get_object(value);
            break;

        case WORKSPACE_GROUP_PROP_WORKSPACE_MANAGER:
            group->priv->workspace_manager = g_value_get_object(value);
            break;

        case WORKSPACE_GROUP_PROP_WORKSPACES:
        case WORKSPACE_GROUP_PROP_ACTIVE_WORKSPACE:
        case WORKSPACE_GROUP_PROP_MONITORS:
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_group_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(obj);

    switch (prop_id) {
        case PROP_HANDLE:
            g_value_set_pointer(value, group->priv->handle);
            break;

        case WORKSPACE_GROUP_PROP_SCREEN:
            g_value_set_object(value, group->priv->screen);
            break;

        case WORKSPACE_GROUP_PROP_WORKSPACE_MANAGER:
            g_value_set_object(value, group->priv->workspace_manager);
            break;

        case WORKSPACE_GROUP_PROP_WORKSPACES:
            g_value_set_pointer(value, group->priv->workspaces);
            break;

        case WORKSPACE_GROUP_PROP_ACTIVE_WORKSPACE:
            g_value_set_object(value, group->priv->active_workspace);
            break;

        case WORKSPACE_GROUP_PROP_MONITORS:
            g_value_set_pointer(value, group->priv->monitors);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_group_wayland_finalize(GObject *obj) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(obj);

    g_signal_handlers_disconnect_by_func(group->priv->screen, screen_monitor_added, group);
    g_signal_handlers_disconnect_by_func(group->priv->screen, screen_monitor_removed, group);

    ext_workspace_group_handle_v1_destroy(group->priv->handle);

    g_list_free(group->priv->workspaces);
    g_list_free(group->priv->monitors);
    g_hash_table_destroy(group->priv->advertised_outputs);

    G_OBJECT_CLASS(xfw_workspace_group_wayland_parent_class)->finalize(obj);
}

static void
xfw_workspace_group_wayland_workspace_group_init(XfwWorkspaceGroupIface *iface) {
    iface->get_capabilities = xfw_workspace_group_wayland_get_capabilities;
    iface->get_workspace_count = xfw_workspace_group_wayland_get_workspace_count;
    iface->list_workspaces = xfw_workspace_group_wayland_list_workspaces;
    iface->get_active_workspace = xfw_workspace_group_wayland_get_active_workspace;
    iface->get_monitors = xfw_workspace_group_wayland_get_monitors;
    iface->get_workspace_manager = xfw_workspace_group_wayland_get_workspace_manager;
    iface->create_workspace = xfw_workspace_group_wayland_create_workspace;
    iface->move_viewport = xfw_workspace_group_wayland_move_viewport;
    iface->set_layout = xfw_workspace_group_wayland_set_layout;
}

static XfwWorkspaceGroupCapabilities
xfw_workspace_group_wayland_get_capabilities(XfwWorkspaceGroup *group) {
    return XFW_WORKSPACE_GROUP_WAYLAND(group)->priv->capabilities;
}

static guint
xfw_workspace_group_wayland_get_workspace_count(XfwWorkspaceGroup *group) {
    return g_list_length(XFW_WORKSPACE_GROUP_WAYLAND(group)->priv->workspaces);
}

static GList *
xfw_workspace_group_wayland_list_workspaces(XfwWorkspaceGroup *group) {
    return XFW_WORKSPACE_GROUP_WAYLAND(group)->priv->workspaces;
}

static XfwWorkspace *
xfw_workspace_group_wayland_get_active_workspace(XfwWorkspaceGroup *group) {
    return XFW_WORKSPACE_GROUP_WAYLAND(group)->priv->active_workspace;
}

static GList *
xfw_workspace_group_wayland_get_monitors(XfwWorkspaceGroup *group) {
    return XFW_WORKSPACE_GROUP_WAYLAND(group)->priv->monitors;
}

static XfwWorkspaceManager *
xfw_workspace_group_wayland_get_workspace_manager(XfwWorkspaceGroup *group) {
    return XFW_WORKSPACE_GROUP_WAYLAND(group)->priv->workspace_manager;
}

static gboolean
xfw_workspace_group_wayland_create_workspace(XfwWorkspaceGroup *group, const gchar *name, GError **error) {
    XfwWorkspaceGroupWayland *wgroup = XFW_WORKSPACE_GROUP_WAYLAND(group);
    if ((wgroup->priv->capabilities & XFW_WORKSPACE_GROUP_CAPABILITIES_CREATE_WORKSPACE) != 0) {
        ext_workspace_group_handle_v1_create_workspace(wgroup->priv->handle, name);
        return TRUE;
    } else {
        if (error) {
            *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This workspace group does not support creating new workspaces");
        }
        return FALSE;
    }
}

static gboolean
xfw_workspace_group_wayland_move_viewport(XfwWorkspaceGroup *group, gint x, gint y, GError **error) {
    if (error) {
        *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This workspace group does not support moving viewports");
    }
    return FALSE;
}

static gboolean
xfw_workspace_group_wayland_set_layout(XfwWorkspaceGroup *group, gint rows, gint columns, GError **error) {
    if (error) {
        *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This workspace group does not support setting a layout");
    }
    return FALSE;
}

static void
group_capabilities(void *data, struct ext_workspace_group_handle_v1 *wl_group, uint32_t wl_capabilities) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(data);
    XfwWorkspaceGroupCapabilities old_capabilities = group->priv->capabilities;
    XfwWorkspaceGroupCapabilities changed_mask;
    XfwWorkspaceGroupCapabilities new_capabilities = XFW_WORKSPACE_GROUP_CAPABILITIES_NONE;

    if ((wl_capabilities & EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE) != 0) {
        new_capabilities |= XFW_WORKSPACE_GROUP_CAPABILITIES_CREATE_WORKSPACE;
    }

    group->priv->capabilities = new_capabilities;
    changed_mask = old_capabilities ^ new_capabilities;
    if (changed_mask != 0) {
        g_object_notify(G_OBJECT(group), "capabilities");
        g_signal_emit_by_name(group, "capabilities-changed", changed_mask, new_capabilities);
    }
}

static void
group_output_enter(void *data, struct ext_workspace_group_handle_v1 *wl_group, struct wl_output *output) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(data);

    g_hash_table_add(group->priv->advertised_outputs, output);

    for (GList *l = xfw_screen_get_monitors(group->priv->screen); l != NULL; l = l->next) {
        XfwMonitorWayland *monitor = XFW_MONITOR_WAYLAND(l->data);
        if (_xfw_monitor_wayland_get_wl_output(monitor) == output
            && g_list_find(group->priv->monitors, monitor) == NULL)
        {
            group->priv->monitors = g_list_append(group->priv->monitors, monitor);
            g_signal_emit_by_name(group, "monitor-added", monitor);
            g_signal_emit_by_name(group, "monitors-changed");
            break;
        }
    }
}

static void
group_output_leave(void *data, struct ext_workspace_group_handle_v1 *wl_group, struct wl_output *output) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(data);

    g_hash_table_remove(group->priv->advertised_outputs, output);

    for (GList *l = xfw_screen_get_monitors(group->priv->screen); l != NULL; l = l->next) {
        XfwMonitorWayland *monitor = XFW_MONITOR_WAYLAND(l->data);
        if (_xfw_monitor_wayland_get_wl_output(monitor) == output) {
            group->priv->monitors = g_list_remove(group->priv->monitors, monitor);
            g_signal_emit_by_name(group, "monitor-removed", monitor);
            g_signal_emit_by_name(group, "monitors-changed");
            break;
        }
    }
}

static void
group_workspace_enter(void *data, struct ext_workspace_group_handle_v1 *wl_group, struct ext_workspace_handle_v1 *wl_workspace) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(data);
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(wl_proxy_get_user_data((struct wl_proxy *)wl_workspace));
    if (g_list_find(group->priv->workspaces, workspace) == NULL) {
        group->priv->workspaces = g_list_append(group->priv->workspaces, workspace);
        _xfw_workspace_wayland_set_workspace_group(workspace, XFW_WORKSPACE_GROUP(group));
        g_signal_emit_by_name(group, "workspace-added", workspace);
    }
}

static void
group_workspace_leave(void *data, struct ext_workspace_group_handle_v1 *wl_group, struct ext_workspace_handle_v1 *wl_workspace) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(data);
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(wl_proxy_get_user_data((struct wl_proxy *)wl_workspace));
    GList *link = g_list_find(group->priv->workspaces, workspace);
    if (link != NULL) {
        group->priv->workspaces = g_list_delete_link(group->priv->workspaces, link);
        _xfw_workspace_wayland_set_workspace_group(workspace, NULL);
        g_signal_emit_by_name(group, "workspace-removed", workspace);
    }
}

static void
group_removed(void *data, struct ext_workspace_group_handle_v1 *wl_group) {
    XfwWorkspaceGroupWayland *group = XFW_WORKSPACE_GROUP_WAYLAND(data);
    g_signal_emit(group, group_signals[SIGNAL_DESTROYED], 0);
}

struct ext_workspace_group_handle_v1 *
_xfw_workspace_group_wayland_get_handle(XfwWorkspaceGroupWayland *group) {
    return group->priv->handle;
}

static void
screen_monitor_added(XfwScreen *screen, XfwMonitor *monitor, XfwWorkspaceGroupWayland *group) {
    struct wl_output *output = _xfw_monitor_wayland_get_wl_output(XFW_MONITOR_WAYLAND(monitor));
    if (g_hash_table_contains(group->priv->advertised_outputs, output)) {
        group_output_enter(group, group->priv->handle, output);
    }
}

static void
screen_monitor_removed(XfwScreen *screen, XfwMonitor *monitor, XfwWorkspaceGroupWayland *group) {
    group_output_leave(group, group->priv->handle, _xfw_monitor_wayland_get_wl_output(XFW_MONITOR_WAYLAND(monitor)));
}

void
_xfw_workspace_group_wayland_set_active_workspace(XfwWorkspaceGroupWayland *group, XfwWorkspace *workspace) {
    if (group->priv->active_workspace != workspace) {
        XfwWorkspace *old_workspace = group->priv->active_workspace;
        group->priv->active_workspace = workspace;
        g_object_notify(G_OBJECT(group), "active-workspace");
        g_signal_emit_by_name(group, "active-workspace-changed", old_workspace);
    }
}
