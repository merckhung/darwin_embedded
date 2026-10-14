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

/**
 * SECTION:xfw-workspace-group
 * @title: XfwWorkspaceGroup
 * @short_description: A group of workspaces
 * @stability: Unstable
 * @include: libxfce4windowing/libxfce4windowing.h
 *
 * Workspaces may be arranged in groups, and groups may be present on different
 * monitors.  The #XfwWorkspaceGroup can create and enumerate workspaces, as
 * well as provide notifications when workspaces are created and destroyed.
 *
 * Each workspace group may have an active workspace.
 *
 * Workspace groups are displayed on a list of zero or more monitors, and
 * have viewport coordinates.
 *
 * Note that #XfwWorkspaceGroup is actually an interface; when obtaining an
 * instance, an instance of a windowing-environment-specific object that
 * implements this interface will be returned.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gdk/gdk.h>

#include "libxfce4windowing-private.h"
#include "xfw-marshal.h"
#include "xfw-monitor.h"
#include "xfw-screen.h"
#include "xfw-workspace-group-private.h"
#include "xfw-workspace-manager.h"
#include "libxfce4windowing-visibility.h"

G_DEFINE_INTERFACE(XfwWorkspaceGroup, xfw_workspace_group, G_TYPE_OBJECT)

G_DEFINE_FLAGS_TYPE(XfwWorkspaceGroupCapabilities, xfw_workspace_group_capabilities,
                    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_GROUP_CAPABILITIES_NONE, "none"),
                    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_GROUP_CAPABILITIES_CREATE_WORKSPACE, "create-workspace"),
                    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_GROUP_CAPABILITIES_MOVE_VIEWPORT, "move-viewport"),
                    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_GROUP_CAPABILITIES_SET_LAYOUT, "set-layout"))

static void
xfw_workspace_group_default_init(XfwWorkspaceGroupIface *iface) {
    /**
     * XfwWorkspaceGroup::capabilities-changed:
     * @group: the object which received the signal.
     * @changed_mask: a bitfield representing the capabilities that have changed.
     * @new_capabilities: a bitfield of the new capabilities.
     *
     * Emitted when capabilities have changed on @group.
     **/
    g_signal_new("capabilities-changed",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, capabilities_changed),
                 NULL, NULL,
                 xfw_marshal_VOID__FLAGS_FLAGS,
                 G_TYPE_NONE, 2,
                 XFW_TYPE_WORKSPACE_GROUP_CAPABILITIES,
                 XFW_TYPE_WORKSPACE_GROUP_CAPABILITIES);

    /**
     * XfwWorkspaceGroup::active-workspace-changed:
     * @group: the object which received the signal.
     * @previously_active_workspace: (nullable): the previously active
     *                                           #XfwWorkspace, or %NULL.
     *
     * Emitted when the active workspace of @group changes.
     **/
    g_signal_new("active-workspace-changed",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, active_workspace_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE, 1,
                 XFW_TYPE_WORKSPACE);

    /**
     * XfwWorkspaceGroup::monitor-added:
     * @group: the object which received the signal.
     * @monitor: a #XfwMonitor.
     *
     * Emitted when @group is added to a new monitor.
     **/
    g_signal_new("monitor-added",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, monitor_added),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE, 1,
                 XFW_TYPE_MONITOR);

    /**
     * XfwWorkspaceGroup::monitor-removed:
     * @group: the object which received the signal.
     * @monitor: a #XfwMonitor.
     *
     * Emitted when @group is removed from a monitor.
     **/
    g_signal_new("monitor-removed",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, monitor_removed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE, 1,
                 XFW_TYPE_MONITOR);

    /**
     * XfwWorkspaceGroup::monitors-changed:
     * @group: the object which received the signal.
     *
     * Emitted when @group moves to a new set of monitors.
     **/
    g_signal_new("monitors-changed",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, monitors_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWorkspaceGroup::workspace-added:
     * @group: the object which received the signal.
     * @workspace: the #XfwWorkspace added to the group.
     *
     * Emitted when @workspace joins @group.
     */
    g_signal_new("workspace-added",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, workspace_added),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE, 1,
                 XFW_TYPE_WORKSPACE);

    /**
     * XfwWorkspaceGroup::workspace-removed:
     * @group: the object which received the signal.
     * @workspace: the #XfwWorkspace removed from the group.
     *
     * Emitted when @workspace leaves @group.
     */
    g_signal_new("workspace-removed",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, workspace_removed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE, 1,
                 XFW_TYPE_WORKSPACE);

    /**
     * XfwWorkspaceGroup::viewports-changed:
     * @group: the object which recieved the signal.
     *
     * Emitted when @group's viewport coordinates have changed.
     **/
    g_signal_new("viewports-changed",
                 XFW_TYPE_WORKSPACE_GROUP,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceGroupIface, viewports_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWorkspaceGroup:screen:
     *
     * The #XfwScreen that owns this #XfwWorkspaceGroup.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_object("screen",
                                                            "screen",
                                                            "screen",
                                                            XFW_TYPE_SCREEN,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspaceGroup:workspace-manager:
     *
     * The #XfwWorkspaceManager instance that manages this #XfwWorkspaceGroup.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_object("workspace-manager",
                                                            "workspace-manager",
                                                            "workspace-manager",
                                                            XFW_TYPE_WORKSPACE_MANAGER,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspaceGroup:workspaces: (type GList(XfwWorkspace))
     *
     * The list of #XfwWorkspace in this #XfwWorkspaceGroup.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_pointer("workspaces",
                                                             "workspaces",
                                                             "workspaces",
                                                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspaceGroup:active-workspace:
     *
     * The active #XfwWorkspace on this #XfwWorkspaceGroup, or %NULL.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_object("active-workspace",
                                                            "active-workspace",
                                                            "active-workspace",
                                                            XFW_TYPE_WORKSPACE,
                                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspaceGroup:monitors: (type GList(XfwMonitor))
     *
     * The list of #XfwMonitor this #XfwWorkspaceGroup is displayed on.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_pointer("monitors",
                                                             "monitors",
                                                             "monitors",
                                                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

/**
 * xfw_workspace_group_get_capabilities:
 * @group: an #XfwWorkspaceGroup.
 *
 * Returns a bitfield describing operations allowed on this @group.
 *
 * Return value: an #XfwWorkspaceGroupCapabilities bitfield.
 **/
XfwWorkspaceGroupCapabilities
xfw_workspace_group_get_capabilities(XfwWorkspaceGroup *group) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), XFW_WORKSPACE_GROUP_CAPABILITIES_NONE);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->get_capabilities)(group);
}

/**
 * xfw_workspace_group_get_workspace_count:
 * @group: an #XfwWorkspaceGroup.
 *
 * Fetches the number of workspaces in @group.
 *
 * Return value: an unsigned integer describing the number of workspaces.
 **/
guint
xfw_workspace_group_get_workspace_count(XfwWorkspaceGroup *group) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), 0);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->get_workspace_count)(group);
}

/**
 * xfw_workspace_group_list_workspaces:
 * @group: an #XfwWorkspaceGroup.
 *
 * Lists the workspaces in @group.
 *
 * Return value: (nullable) (element-type XfwWorkspace) (transfer none):
 * the list of #XfwWorkspace in @group, or %NULL if there are no workspaces.
 * The list and its contents are owned by @group.
 **/
GList *
xfw_workspace_group_list_workspaces(XfwWorkspaceGroup *group) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), NULL);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->list_workspaces)(group);
}

/**
 * xfw_workspace_group_get_active_workspace:
 * @group: an #XfwWorkspaceGroup.
 *
 * Gets the active workspace on @group, if there is one.
 *
 * Return value: (nullable) (transfer none): an #XfwWorkspace, or %NULL
 * if there is no active workspace.
 **/
XfwWorkspace *
xfw_workspace_group_get_active_workspace(XfwWorkspaceGroup *group) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), NULL);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->get_active_workspace)(group);
}

/**
 * xfw_workspace_group_get_monitors:
 * @group: an #XfwWorkspaceGroup.
 *
 * Lists the physical monitors that this workspace group displays on.
 *
 * Return value: (nullable) (element-type XfwMonitor) (transfer none):
 * A list of #XfwMonitor, or %NULL if @group is not displayed on any
 * monitors.  The list and its contents are owned by @group.
 **/
GList *
xfw_workspace_group_get_monitors(XfwWorkspaceGroup *group) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), NULL);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->get_monitors)(group);
}

/**
 * xfw_workspace_group_get_workspace_manager:
 * @group: an #XfwWorkspaceGroup.
 *
 * Fetches the #XfwWorkspaceManager instance that owns @group.
 *
 * Return value: (not nullable) (transfer none): a #XfwWorkspaceManager,
 * owned by @group.
 **/
XfwWorkspaceManager *
xfw_workspace_group_get_workspace_manager(XfwWorkspaceGroup *group) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), NULL);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->get_workspace_manager)(group);
}

/**
 * xfw_workspace_group_create_workspace:
 * @group: an #XfwWorkspaceGroup.
 * @name: a name for the new workspace.
 * @error: (out callee-allocates): a location to store a possible error.
 *
 * Attempts to create a new workspace on @group.  Typically, the new workspace
 * will be appended to the existing list of workspaces.
 *
 * On failure, @error (if provided) will be set to a description of the error
 * that occurred.
 *
 * Return value: %TRUE if workspace creation succeeded, %FALSE otherwise.  If
 * %FALSE, and @error is non-%NULL, an error will be returned that must be
 * freed using #g_error_free().
 **/
gboolean
xfw_workspace_group_create_workspace(XfwWorkspaceGroup *group, const gchar *name, GError **error) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), FALSE);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->create_workspace)(group, name, error);
}

/**
 * xfw_workspace_group_move_viewport:
 * @group: an #XfwWorkspaceGroup.
 * @x: a coordinate in the horizontal direction.
 * @y: a coordinate in the vertical direction.
 * @error: (out callee-allocates): a location to store a possible error.
 *
 * Moves the workspace group to a new location, and possibly a new monitor.
 *
 * On failure, @error (if provided) will be set to a description of the error
 * that occurred.
 *
 * Return value: %TRUE if moving the workspace group succeeded, %FALSE
 * otherwise.  If %FALSE, and @error is non-%NULL, an error will be returned
 * that must be freed using #g_error_free().
 **/
gboolean
xfw_workspace_group_move_viewport(XfwWorkspaceGroup *group, gint x, gint y, GError **error) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), FALSE);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->move_viewport)(group, x, y, error);
}

/**
 * xfw_workspace_group_set_layout:
 * @group: an #XfwWorkspaceGroup.
 * @rows: the new numbers of rows.
 * @columns: the new number of columns.
 * @error: (out callee-allocates): a location to store a possible error.
 *
 * Sets the layout of @group to @rows by @columns.
 *
 * Note that this will not change the number of workspaces if the new layout
 * implies a larger number of workspaces than currently exists.
 *
 * On failure, @error (if provided) will be set to a description of the error
 * that occurred.
 *
 * Return value: %TRUE if changing the layout of @group succeede, %FALSE
 * otherwise.  If %FALSE, and @error is non-%NULL, an error will be returned
 * that must be freed using #g_error_free().
 **/
gboolean
xfw_workspace_group_set_layout(XfwWorkspaceGroup *group, gint rows, gint columns, GError **error) {
    XfwWorkspaceGroupIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE_GROUP(group), FALSE);
    iface = XFW_WORKSPACE_GROUP_GET_IFACE(group);
    return (*iface->move_viewport)(group, rows, columns, error);
}

void
_xfw_workspace_group_install_properties(GObjectClass *gklass) {
    g_object_class_override_property(gklass, WORKSPACE_GROUP_PROP_SCREEN, "screen");
    g_object_class_override_property(gklass, WORKSPACE_GROUP_PROP_WORKSPACE_MANAGER, "workspace-manager");
    g_object_class_override_property(gklass, WORKSPACE_GROUP_PROP_WORKSPACES, "workspaces");
    g_object_class_override_property(gklass, WORKSPACE_GROUP_PROP_ACTIVE_WORKSPACE, "active-workspace");
    g_object_class_override_property(gklass, WORKSPACE_GROUP_PROP_MONITORS, "monitors");
}

#define __XFW_WORKSPACE_GROUP_C__
#include "libxfce4windowing-visibility.c"
