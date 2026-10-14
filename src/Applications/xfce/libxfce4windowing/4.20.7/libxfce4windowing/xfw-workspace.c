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
 * SECTION:xfw-workspace
 * @title: XfwWorkspace
 * @short_description: A workspace that is part of a workspace group
 * @stability: Unstable
 * @include: libxfce4windowing/libxfce4windowing.h
 *
 * #XfwWorkspace represents a single workspace within a workspace group.  A
 * workspace is usually a collection of windows that are shown together on the
 * desktop when that workspace is the active workspace.
 *
 * An instance of #XfwWorkspace can be used to obtain information about the
 * workspace, such as its name, position in the group, and capabilities.  The
 * workspace can also be activated or removed.
 *
 * Note that #XfwWorkspace is actually an interface; when obtaining an
 * instance, an instance of a windowing-environment-specific object that
 * implements this interface will be returned.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>

#include "libxfce4windowing-private.h"
#include "xfw-marshal.h"
#include "xfw-window.h"
#include "xfw-workspace-group.h"
#include "xfw-workspace-private.h"
#include "libxfce4windowing-visibility.h"

G_DEFINE_INTERFACE(XfwWorkspace, xfw_workspace, G_TYPE_OBJECT)

G_DEFINE_FLAGS_TYPE(
    XfwWorkspaceState, xfw_workspace_state,
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_STATE_NONE, "none"),
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_STATE_ACTIVE, "active"),
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_STATE_URGENT, "urgent"),
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_STATE_HIDDEN, "hidden"),
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_STATE_VIRTUAL, "virtual"))

G_DEFINE_FLAGS_TYPE(
    XfwWorkspaceCapabilities, xfw_workspace_capabilities,
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_CAPABILITIES_NONE, "none"),
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_CAPABILITIES_ACTIVATE, "activate"),
    G_DEFINE_ENUM_VALUE(XFW_WORKSPACE_CAPABILITIES_REMOVE, "remove"))

static void
xfw_workspace_default_init(XfwWorkspaceIface *iface) {
    /**
     * XfwWorkspace::name-changed:
     * @workspace: the object which received the signal.
     *
     * Emitted when @workspace's name changes.
     **/
    g_signal_new("name-changed",
                 XFW_TYPE_WORKSPACE,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceIface, name_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWorkspace::capabilities-changed:
     * @workspace: the object which received the signal.
     * @changed_mask: a bitfield representing the capabilities that have changed.
     * @new_capabilities: a bitfield of the new capabilities.
     *
     * Emitted when @workspace's capabilities change.
     **/
    g_signal_new("capabilities-changed",
                 XFW_TYPE_WORKSPACE,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceIface, capabilities_changed),
                 NULL, NULL,
                 xfw_marshal_VOID__FLAGS_FLAGS,
                 G_TYPE_NONE, 2,
                 XFW_TYPE_WORKSPACE_CAPABILITIES,
                 XFW_TYPE_WORKSPACE_CAPABILITIES);

    /**
     * XfwWorkspace::state-changed:
     * @workspace: the object which received the signal.
     * @changed_mask: a bitfield representing the state bits that have changed.
     * @new_state: a bitfield of the new state.
     *
     * Emitted when @workspace's state changes.
     **/
    g_signal_new("state-changed",
                 XFW_TYPE_WORKSPACE,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceIface, state_changed),
                 NULL, NULL,
                 xfw_marshal_VOID__FLAGS_FLAGS,
                 G_TYPE_NONE, 2,
                 XFW_TYPE_WORKSPACE_STATE,
                 XFW_TYPE_WORKSPACE_STATE);

    /**
     * XfwWorkspace::group-changed:
     * @workspace: the object which received the signal.
     * @previous_group: the group @workspace was previously in, or %NULL.
     *
     * Emitted when @workspace is assigned to an #XfwWorkspaceGroup.
     **/
    g_signal_new("group-changed",
                 XFW_TYPE_WORKSPACE,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWorkspaceIface, group_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE, 1,
                 XFW_TYPE_WORKSPACE_GROUP);

    /**
     * XfwWorkspace:group:
     *
     * The #XfwWorkspaceGroup that this workspace is a member of, if any.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_object("group",
                                                            "group",
                                                            "group",
                                                            XFW_TYPE_WORKSPACE_GROUP,
                                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:id:
     *
     * The opaque ID of this workspace.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_string("id",
                                                            "id",
                                                            "id",
                                                            "",
                                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:name:
     *
     * The human-readable name of this workspace.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_string("name",
                                                            "name",
                                                            "name",
                                                            "",
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:capabilities:
     *
     * The #XfwWorkspaceCapabilities bitfield for this workspace.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_flags("capabilities",
                                                           "capabilities",
                                                           "capabilities",
                                                           XFW_TYPE_WORKSPACE_CAPABILITIES,
                                                           XFW_WORKSPACE_CAPABILITIES_NONE,
                                                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:state:
     *
     * The #XfwWorkspaceState bitfield for this workspace.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_flags("state",
                                                           "state",
                                                           "state",
                                                           XFW_TYPE_WORKSPACE_STATE,
                                                           XFW_WORKSPACE_STATE_NONE,
                                                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:number:
     *
     * The ordinal number of this workspace.
     **/
    g_object_interface_install_property(iface,
                                        g_param_spec_uint("number",
                                                          "number",
                                                          "number",
                                                          0, UINT_MAX, 0,
                                                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:layout-row:
     *
     * The x-coordinate of the workspace on a 2D grid.
     */
    g_object_interface_install_property(iface,
                                        g_param_spec_int("layout-row",
                                                         "layout-row",
                                                         "layout-row",
                                                         -1, G_MAXINT, -1,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:layout-column:
     *
     * The y-coordinate of the workspace on a 2D grid.
     */
    g_object_interface_install_property(iface,
                                        g_param_spec_int("layout-column",
                                                         "layout-column",
                                                         "layout-column",
                                                         -1, G_MAXINT, -1,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWorkspace:geometry:
     *
     * The position and size of the workspace on the desktop, in screen
     * coordinates.
     *
     * Since: 4.20.6
     */
    g_object_interface_install_property(iface,
                                        g_param_spec_boxed("geometry",
                                                           "geometry",
                                                           "geometry",
                                                           GDK_TYPE_RECTANGLE,
                                                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

/**
 * xfw_workspace_get_id:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches this workspace's opaque ID.
 *
 * Return value: (not nullable) (transfer none): A UTF-8 formatted string,
 * owned by @workspace.
 **/
const gchar *
xfw_workspace_get_id(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), NULL);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_id)(workspace);
}

/**
 * xfw_workspace_get_name:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches this workspace's human-readable name.
 *
 * Return value: (not nullable) (transfer none): A UTF-8 formatted string,
 * owned by @workspace.
 **/
const gchar *
xfw_workspace_get_name(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), NULL);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_name)(workspace);
}

/**
 * xfw_workspace_get_capabilities:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches this workspace's capabilities bitfield.
 *
 * The bitfield describes what operations are available on this workspace.
 *
 * Return value: a #XfwWorkspaceCapabilities bitfield.
 **/
XfwWorkspaceCapabilities
xfw_workspace_get_capabilities(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), XFW_WORKSPACE_CAPABILITIES_NONE);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_capabilities)(workspace);
}

/**
 * xfw_workspace_get_state:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches this workspace's state bitfield.
 *
 * Return value: a #XfwWorkspaceState bitfield.
 **/
XfwWorkspaceState
xfw_workspace_get_state(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), XFW_WORKSPACE_STATE_NONE);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_state)(workspace);
}

/**
 * xfw_workspace_get_number:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches the ordinal number of this workspace.
 *
 * The number can be used to order workspaces in a UI representation.
 *
 * On X11, this number should be stable across runs of your application.
 *
 * On Wayland, this number depends on the order in which the compositor
 * advertises the workspaces.  This order may be stable, but may not be.
 *
 * Return value: a non-negative, 0-indexed integer.
 **/
guint
xfw_workspace_get_number(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), 0);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_number)(workspace);
}

/**
 * xfw_workspace_get_workspace_group:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches the group this workspace belongs to, if any.
 *
 * Return value: (nullable) (transfer none): a #XfwWorkspaceGroup
 * instance, owned by @workspace, or %NULL if the workspace is not a member of
 * any groups.
 **/
XfwWorkspaceGroup *
xfw_workspace_get_workspace_group(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), NULL);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_workspace_group)(workspace);
}

/**
 * xfw_workspace_get_layout_row:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches the row this workspace belongs to in the workspace's group.
 *
 * This information can be used to lay out workspaces in a grid in a pager
 * UI, for example.
 *
 * Return value: a non-negative, 0-indexed integer.
 **/
gint
xfw_workspace_get_layout_row(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), 0);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_layout_row)(workspace);
}

/**
 * xfw_workspace_get_layout_column:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches the column this workspace belongs to in the workspace's group.
 *
 * This information can be used to lay out workspaces in a grid in a pager
 * UI, for example.
 *
 * Return value: a non-negative, 0-indexed integer.
 **/
gint
xfw_workspace_get_layout_column(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), 0);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_layout_column)(workspace);
}

/**
 * xfw_workspace_get_neighbor:
 * @workspace: an #XfwWorkspace.
 * @direction: an #XfwDirection.
 *
 * Fetches the workspace that resides in @direction from the @workspace, if
 * any.  If workspace is on the edge of the layout, and @direction points off
 * the edge of the layout, will return %NULL.
 *
 * Return value: (nullable) (transfer none): a #XfwWorkspace, owned by
 * the parent @group, or %NULL if no workspace exists in @direction.
 **/
XfwWorkspace *
xfw_workspace_get_neighbor(XfwWorkspace *workspace, XfwDirection direction) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), NULL);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_neighbor)(workspace, direction);
}

/**
 * xfw_workspace_get_geometry:
 * @workspace: an #XfwWorkspace.
 *
 * Fetches the position and size of the workspace in screen coordinates.
 *
 * The values in the returned #GdkRectangle are owned by @workspace and should
 * not be modified.
 *
 * Return value: (not nullable) (transfer none): a #GdkRectangle, owned by
 * @workspace.
 **/
GdkRectangle *
xfw_workspace_get_geometry(XfwWorkspace *workspace) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), NULL);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->get_geometry)(workspace);
}

/**
 * xfw_workspace_activate:
 * @workspace: an #XfwWorkspace.
 * @error: (out callee-allocates): a location to store a possible error.
 *
 * Attempts to set @workspace as the active workspace in its group.
 *
 * On failure, @error (if provided) will be set to a description of the error
 * that occurred.
 *
 * Return value: %TRUE if workspace activation succeeded, %FALSE otherwise.  If
 * %FALSE, and @error is non-%NULL, an error will be returned that must be
 * freed using #g_error_free().
 **/
gboolean
xfw_workspace_activate(XfwWorkspace *workspace, GError **error) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), FALSE);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->activate)(workspace, error);
}

/**
 * xfw_workspace_remove:
 * @workspace: an #XfwWorkspace.
 * @error: (out callee-allocates): a location to store a possible error.
 *
 * Attempts to remove @workspace from its group.
 *
 * On failure, @error (if provided) will be set to a description of the error
 * that occurred.
 *
 * Return value: %TRUE if workspace removal succeeded, %FALSE otherwise.  If
 * %FALSE, and @error is non-%NULL, an error will be returned that must be
 * freed using #g_error_free().
 **/
gboolean
xfw_workspace_remove(XfwWorkspace *workspace, GError **error) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), FALSE);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->remove)(workspace, error);
}

/**
 * xfw_workspace_assign_to_workspace_group:
 * @workspace: an #XfwWorkspace.
 * @group: an #XfwWorkspaceGroup.
 * @error: (out callee-allocates): a location to store a possible error.
 *
 * Attempts to assign @workspace to @group.
 *
 * On failure, @error (if provided) will be set to a description of the error
 * that occurred.
 *
 * Return value: %TRUE if workspace assignment succeeded, %FALSE otherwise.
 * If %FALSE, and @error is non-%NULL, an error will be returned that must be
 * freed using g_error_free().
 **/
gboolean
xfw_workspace_assign_to_workspace_group(XfwWorkspace *workspace, XfwWorkspaceGroup *group, GError **error) {
    XfwWorkspaceIface *iface;
    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), FALSE);
    iface = XFW_WORKSPACE_GET_IFACE(workspace);
    return (*iface->assign_to_workspace_group)(workspace, group, error);
}

void
_xfw_workspace_install_properties(GObjectClass *gklass) {
    g_object_class_override_property(gklass, WORKSPACE_PROP_GROUP, "group");
    g_object_class_override_property(gklass, WORKSPACE_PROP_ID, "id");
    g_object_class_override_property(gklass, WORKSPACE_PROP_NAME, "name");
    g_object_class_override_property(gklass, WORKSPACE_PROP_CAPABILITIES, "capabilities");
    g_object_class_override_property(gklass, WORKSPACE_PROP_STATE, "state");
    g_object_class_override_property(gklass, WORKSPACE_PROP_NUMBER, "number");
    g_object_class_override_property(gklass, WORKSPACE_PROP_LAYOUT_ROW, "layout-row");
    g_object_class_override_property(gklass, WORKSPACE_PROP_LAYOUT_COLUMN, "layout-column");
    g_object_class_override_property(gklass, WORKSPACE_PROP_GEOMETRY, "geometry");
}

#define __XFW_WORKSPACE_C__
#include "libxfce4windowing-visibility.c"
