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

#include <gdk/gdkwayland.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "protocols/ext-workspace-v1-client.h"

#include "libxfce4windowing-private.h"
#include "xfw-screen-private.h"
#include "xfw-workspace-group-wayland.h"
#include "xfw-workspace-manager-dummy.h"
#include "xfw-workspace-manager-private.h"
#include "xfw-workspace-manager-wayland.h"
#include "xfw-workspace-wayland.h"

enum {
    PROP_0,
    PROP_WL_MANAGER,
};

struct _XfwWorkspaceManagerWaylandPrivate {
    struct ext_workspace_manager_v1 *handle;
    XfwScreen *screen;
    GList *groups;
    GList *workspaces;
};

static void xfw_workspace_manager_wayland_manager_init(XfwWorkspaceManagerIface *iface);
static void xfw_workspace_manager_wayland_constructed(GObject *obj);
static void xfw_workspace_manager_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_workspace_manager_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_workspace_manager_wayland_finalize(GObject *obj);
static GList *xfw_workspace_manager_wayland_list_workspace_groups(XfwWorkspaceManager *manager);
static GList *xfw_workspace_manager_wayland_list_workspaces(XfwWorkspaceManager *manager);

static void manager_workspace_group(void *data, struct ext_workspace_manager_v1 *manager, struct ext_workspace_group_handle_v1 *group);
static void manager_workspace(void *data, struct ext_workspace_manager_v1 *manager, struct ext_workspace_handle_v1 *workspace);
static void manager_done(void *data, struct ext_workspace_manager_v1 *manager);
static void manager_finished(void *data, struct ext_workspace_manager_v1 *manager);


static const struct ext_workspace_manager_v1_listener manager_listener = {
    .workspace_group = manager_workspace_group,
    .workspace = manager_workspace,
    .done = manager_done,
    .finished = manager_finished,
};

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWorkspaceManagerWayland, xfw_workspace_manager_wayland, G_TYPE_OBJECT,
                              G_ADD_PRIVATE(XfwWorkspaceManagerWayland)
                              G_IMPLEMENT_INTERFACE(XFW_TYPE_WORKSPACE_MANAGER,
                                                    xfw_workspace_manager_wayland_manager_init))

static void
xfw_workspace_manager_wayland_class_init(XfwWorkspaceManagerWaylandClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);

    gklass->constructed = xfw_workspace_manager_wayland_constructed;
    gklass->set_property = xfw_workspace_manager_wayland_set_property;
    gklass->get_property = xfw_workspace_manager_wayland_get_property;
    gklass->finalize = xfw_workspace_manager_wayland_finalize;

    g_object_class_install_property(gklass,
                                    PROP_WL_MANAGER,
                                    g_param_spec_pointer("wl-manager",
                                                         "wl-manager",
                                                         "wl-manager",
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
    _xfw_workspace_manager_install_properties(gklass);
}

static void
xfw_workspace_manager_wayland_manager_init(XfwWorkspaceManagerIface *iface) {
    iface->list_workspace_groups = xfw_workspace_manager_wayland_list_workspace_groups;
    iface->list_workspaces = xfw_workspace_manager_wayland_list_workspaces;
}

static void
xfw_workspace_manager_wayland_init(XfwWorkspaceManagerWayland *manager) {
    manager->priv = xfw_workspace_manager_wayland_get_instance_private(manager);
}

static void
xfw_workspace_manager_wayland_constructed(GObject *obj) {
    XfwWorkspaceManagerWayland *manager = XFW_WORKSPACE_MANAGER_WAYLAND(obj);
    ext_workspace_manager_v1_add_listener(manager->priv->handle, &manager_listener, manager);

    G_OBJECT_CLASS(xfw_workspace_manager_wayland_parent_class)->constructed(obj);
}

static void
xfw_workspace_manager_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWorkspaceManagerWayland *manager = XFW_WORKSPACE_MANAGER_WAYLAND(obj);

    switch (prop_id) {
        case PROP_WL_MANAGER:
            manager->priv->handle = g_value_get_pointer(value);
            break;

        case WORKSPACE_MANAGER_PROP_SCREEN:
            manager->priv->screen = XFW_SCREEN(g_value_get_object(value));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_manager_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWorkspaceManagerWayland *manager = XFW_WORKSPACE_MANAGER_WAYLAND(obj);

    switch (prop_id) {
        case PROP_WL_MANAGER:
            g_value_set_pointer(value, manager->priv->handle);
            break;

        case WORKSPACE_MANAGER_PROP_SCREEN:
            g_value_set_object(value, manager->priv->screen);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_manager_wayland_finalize(GObject *obj) {
    XfwWorkspaceManagerWayland *manager = XFW_WORKSPACE_MANAGER_WAYLAND(obj);
    XfwWorkspaceManagerWaylandPrivate *priv = manager->priv;

    g_list_free_full(priv->workspaces, g_object_unref);
    g_list_free_full(priv->groups, g_object_unref);

    ext_workspace_manager_v1_destroy(priv->handle);

    G_OBJECT_CLASS(xfw_workspace_manager_wayland_parent_class)->finalize(obj);
}

static GList *
xfw_workspace_manager_wayland_list_workspace_groups(XfwWorkspaceManager *manager) {
    XfwWorkspaceManagerWayland *wmanager = XFW_WORKSPACE_MANAGER_WAYLAND(manager);
    return wmanager->priv->groups;
}

static GList *
xfw_workspace_manager_wayland_list_workspaces(XfwWorkspaceManager *manager) {
    XfwWorkspaceManagerWayland *wmanager = XFW_WORKSPACE_MANAGER_WAYLAND(manager);
    return wmanager->priv->workspaces;
}

static void
group_destroyed(XfwWorkspaceGroupWayland *group, XfwWorkspaceManagerWayland *manager) {
    g_signal_handlers_disconnect_by_func(group, group_destroyed, manager);
    manager->priv->groups = g_list_remove(manager->priv->groups, group);
    g_signal_emit_by_name(manager, "workspace-group-destroyed", group);
    g_object_unref(group);
}

static void
manager_workspace_group(void *data, struct ext_workspace_manager_v1 *manager, struct ext_workspace_group_handle_v1 *wl_group) {
    XfwWorkspaceManagerWayland *wmanager = XFW_WORKSPACE_MANAGER_WAYLAND(data);
    XfwWorkspaceGroupWayland *group = g_object_new(XFW_TYPE_WORKSPACE_GROUP_WAYLAND,
                                                   "screen", wmanager->priv->screen,
                                                   "workspace-manager", wmanager,
                                                   "handle", wl_group,
                                                   NULL);
    wmanager->priv->groups = g_list_append(wmanager->priv->groups, group);
    g_signal_connect(group, "destroyed", G_CALLBACK(group_destroyed), wmanager);
    g_signal_emit_by_name(wmanager, "workspace-group-created", group);
}

static void
workspace_destroyed(XfwWorkspace *workspace, XfwWorkspaceManagerWayland *manager) {
    GList *cur;

    g_signal_handlers_disconnect_by_data(workspace, manager);

    cur = g_list_find(manager->priv->workspaces, workspace);
    if (cur != NULL) {
        GList *link = cur;
        cur = cur->next;
        manager->priv->workspaces = g_list_delete_link(manager->priv->workspaces, link);

        for (; cur != NULL; cur = cur->next) {
            XfwWorkspace *cur_workspace = XFW_WORKSPACE(cur->data);
            guint cur_number = xfw_workspace_get_number(cur_workspace);
            _xfw_workspace_wayland_set_number(XFW_WORKSPACE_WAYLAND(cur_workspace), cur_number - 1);
        }
    }

    g_signal_emit_by_name(manager, "workspace-destroyed", workspace);
    g_object_unref(workspace);
}

static void
manager_workspace(void *data, struct ext_workspace_manager_v1 *manager, struct ext_workspace_handle_v1 *wl_workspace) {
    XfwWorkspaceManagerWayland *wmanager = XFW_WORKSPACE_MANAGER_WAYLAND(data);
    XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(g_object_new(XFW_TYPE_WORKSPACE_WAYLAND,
                                                                        "handle", wl_workspace,
                                                                        NULL));
    _xfw_workspace_wayland_set_workspace_manager_handle(workspace, wmanager->priv->handle);
    _xfw_workspace_wayland_set_number(workspace, g_list_length(wmanager->priv->workspaces));
    wmanager->priv->workspaces = g_list_append(wmanager->priv->workspaces, workspace);
    g_signal_connect(workspace, "destroyed", G_CALLBACK(workspace_destroyed), wmanager);
    g_signal_emit_by_name(wmanager, "workspace-created", workspace);
}

static void
manager_done(void *data, struct ext_workspace_manager_v1 *manager) {
}

static void
manager_finished(void *data, struct ext_workspace_manager_v1 *manager) {
}

XfwWorkspaceManager *
_xfw_workspace_manager_wayland_new(XfwScreenWayland *screen, struct ext_workspace_manager_v1 *manager) {
    return g_object_new(XFW_TYPE_WORKSPACE_MANAGER_WAYLAND,
                        "screen", screen,
                        "wl-manager", manager,
                        NULL);
}

XfwWorkspace *
_xfw_workspace_manager_wayland_workspace_for_handle(XfwWorkspaceManagerWayland *manager, struct ext_workspace_handle_v1 *ext_workspace) {
    for (GList *l = manager->priv->workspaces; l != NULL; l = l->next) {
        XfwWorkspaceWayland *workspace = XFW_WORKSPACE_WAYLAND(l->data);
        if (_xfw_workspace_wayland_get_handle(workspace) == ext_workspace) {
            return XFW_WORKSPACE(workspace);
        }
    }
    return NULL;
}
