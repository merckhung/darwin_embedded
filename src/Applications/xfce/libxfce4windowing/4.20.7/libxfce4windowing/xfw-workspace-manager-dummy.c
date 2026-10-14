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

#include "libxfce4windowing-private.h"
#include "xfw-screen.h"
#include "xfw-workspace-dummy.h"
#include "xfw-workspace-group-dummy.h"
#include "xfw-workspace-manager-dummy.h"
#include "xfw-workspace-manager-private.h"

struct _XfwWorkspaceManagerDummyPrivate {
    XfwScreen *screen;
    GList *groups;
    GList *workspaces;
};

static void xfw_workspace_manager_dummy_manager_init(XfwWorkspaceManagerIface *iface);
static void xfw_workspace_manager_dummy_constructed(GObject *obj);
static void xfw_workspace_manager_dummy_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_workspace_manager_dummy_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_workspace_manager_dummy_finalize(GObject *obj);
static GList *xfw_workspace_manager_dummy_list_workspace_groups(XfwWorkspaceManager *manager);
static GList *xfw_workspace_manager_dummy_list_workspaces(XfwWorkspaceManager *manager);

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWorkspaceManagerDummy, xfw_workspace_manager_dummy, G_TYPE_OBJECT,
                              G_ADD_PRIVATE(XfwWorkspaceManagerDummy)
                              G_IMPLEMENT_INTERFACE(XFW_TYPE_WORKSPACE_MANAGER,
                                                    xfw_workspace_manager_dummy_manager_init))

static void
xfw_workspace_manager_dummy_class_init(XfwWorkspaceManagerDummyClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    gklass->constructed = xfw_workspace_manager_dummy_constructed;
    gklass->set_property = xfw_workspace_manager_dummy_set_property;
    gklass->get_property = xfw_workspace_manager_dummy_get_property;
    gklass->finalize = xfw_workspace_manager_dummy_finalize;
    _xfw_workspace_manager_install_properties(gklass);
}

static void
xfw_workspace_manager_dummy_manager_init(XfwWorkspaceManagerIface *iface) {
    iface->list_workspace_groups = xfw_workspace_manager_dummy_list_workspace_groups;
    iface->list_workspaces = xfw_workspace_manager_dummy_list_workspaces;
}

static void
xfw_workspace_manager_dummy_init(XfwWorkspaceManagerDummy *manager) {
    manager->priv = xfw_workspace_manager_dummy_get_instance_private(manager);
}

static void
xfw_workspace_manager_dummy_constructed(GObject *obj) {
    XfwWorkspaceManagerDummy *manager = XFW_WORKSPACE_MANAGER_DUMMY(obj);
    XfwWorkspaceGroupDummy *group;

    manager->priv = xfw_workspace_manager_dummy_get_instance_private(manager);

    group = g_object_new(XFW_TYPE_WORKSPACE_GROUP_DUMMY,
                         "screen", manager->priv->screen,
                         "workspace-manager", manager,
                         "create-workspace-func", NULL,
                         "move-viewport-func", NULL,
                         "set-layout-func", NULL,
                         NULL);
    manager->priv->groups = g_list_append(NULL, group);
    manager->priv->workspaces = g_list_append(NULL, g_object_new(XFW_TYPE_WORKSPACE_DUMMY, NULL));
    _xfw_workspace_dummy_set_workspace_group(XFW_WORKSPACE_DUMMY(manager->priv->workspaces->data), XFW_WORKSPACE_GROUP(group));
    _xfw_workspace_group_dummy_set_workspaces(group, manager->priv->workspaces);
    _xfw_workspace_group_dummy_set_active_workspace(group, XFW_WORKSPACE(manager->priv->workspaces->data));

    G_OBJECT_CLASS(xfw_workspace_manager_dummy_parent_class)->constructed(obj);
}

static void
xfw_workspace_manager_dummy_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWorkspaceManagerDummy *manager = XFW_WORKSPACE_MANAGER_DUMMY(obj);

    switch (prop_id) {
        case WORKSPACE_MANAGER_PROP_SCREEN:
            manager->priv->screen = g_value_get_object(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_manager_dummy_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWorkspaceManagerDummy *manager = XFW_WORKSPACE_MANAGER_DUMMY(obj);

    switch (prop_id) {
        case WORKSPACE_MANAGER_PROP_SCREEN:
            g_value_set_object(value, manager->priv->screen);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_workspace_manager_dummy_finalize(GObject *obj) {
    XfwWorkspaceManagerDummy *manager = XFW_WORKSPACE_MANAGER_DUMMY(obj);

    g_list_free_full(manager->priv->groups, g_object_unref);
    g_list_free_full(manager->priv->workspaces, g_object_unref);

    G_OBJECT_CLASS(xfw_workspace_manager_dummy_parent_class)->finalize(obj);
}

static GList *
xfw_workspace_manager_dummy_list_workspace_groups(XfwWorkspaceManager *manager) {
    return XFW_WORKSPACE_MANAGER_DUMMY(manager)->priv->groups;
}

static GList *
xfw_workspace_manager_dummy_list_workspaces(XfwWorkspaceManager *manager) {
    return XFW_WORKSPACE_MANAGER_DUMMY(manager)->priv->workspaces;
}

XfwWorkspaceManager *
_xfw_workspace_manager_dummy_new(XfwScreen *screen) {
    return g_object_new(XFW_TYPE_WORKSPACE_MANAGER_DUMMY,
                        "screen", screen,
                        NULL);
}
