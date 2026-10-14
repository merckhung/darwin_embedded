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

#ifndef __XFW_WINDOW_H__
#define __XFW_WINDOW_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <gdk/gdk.h>
#include <libxfce4windowing/xfw-application.h>
#include <libxfce4windowing/xfw-seat.h>
#include <libxfce4windowing/xfw-workspace.h>

G_BEGIN_DECLS

/* fwd decl */
typedef struct _XfwScreen XfwScreen;

#define XFW_TYPE_WINDOW (xfw_window_get_type())
G_DECLARE_DERIVABLE_TYPE(XfwWindow, xfw_window, XFW, WINDOW, GObject)

#define XFW_TYPE_WINDOW_TYPE (xfw_window_type_get_type())
#define XFW_TYPE_WINDOW_STATE (xfw_window_state_get_type())
#define XFW_TYPE_WINDOW_CAPABILITIES (xfw_window_capabilities_get_type())

/**
 * XfwWindowState:
 * @XFW_WINDOW_STATE_NONE: window has no state bits set.
 * @XFW_WINDOW_STATE_ACTIVE: window is active (and often has the keyboard
 *                           focus).
 * @XFW_WINDOW_STATE_MINIMIZED: window is minimized/hidden.
 * @XFW_WINDOW_STATE_MAXIMIZED: window is maximized.
 * @XFW_WINDOW_STATE_FULLSCREEN: window is filling the entire screen.
 * @XFW_WINDOW_STATE_SKIP_PAGER: window should not be shown in pagers.
 * @XFW_WINDOW_STATE_SKIP_TASKLIST: window should not be shown in task lists.
 * @XFW_WINDOW_STATE_PINNED: window is shown on al workspaces.
 * @XFW_WINDOW_STATE_SHADED: window is hidden, except for its title bar.
 * @XFW_WINDOW_STATE_ABOVE: window is always shown above other windows.
 * @XFW_WINDOW_STATE_BELOW: window is always shown below other windows.
 * @XFW_WINDOW_STATE_URGENT: window is attempting to get the user's attention.
 *
 * A flags bitfield representing various states the window can hold.
 **/
typedef enum {
    XFW_WINDOW_STATE_NONE = 0,
    XFW_WINDOW_STATE_ACTIVE = (1 << 0),
    XFW_WINDOW_STATE_MINIMIZED = (1 << 1),
    XFW_WINDOW_STATE_MAXIMIZED = (1 << 2),
    XFW_WINDOW_STATE_FULLSCREEN = (1 << 3),
    XFW_WINDOW_STATE_SKIP_PAGER = (1 << 4),
    XFW_WINDOW_STATE_SKIP_TASKLIST = (1 << 5),
    XFW_WINDOW_STATE_PINNED = (1 << 6),
    XFW_WINDOW_STATE_SHADED = (1 << 7),
    XFW_WINDOW_STATE_ABOVE = (1 << 8),
    XFW_WINDOW_STATE_BELOW = (1 << 9),
    XFW_WINDOW_STATE_URGENT = (1 << 10),
} XfwWindowState;

/**
 * XfwWindowCapabilities:
 * @XFW_WINDOW_CAPABILITIES_NONE: window has no capabilities.
 * @XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE: window can be minimized/hidden.
 * @XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE: window can be unminimized/unhidden.
 * @XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE: window can be maximized.
 * @XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE: window can be unmaximized/restored.
 * @XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN: window can be set fullscreen.
 * @XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN: window can be unset fullscreen.
 * @XFW_WINDOW_CAPABILITIES_CAN_SHADE: window can be shaded.
 * @XFW_WINDOW_CAPABILITIES_CAN_UNSHADE: window can be unshaded.
 * @XFW_WINDOW_CAPABILITIES_CAN_MOVE: window can be moved.
 * @XFW_WINDOW_CAPABILITIES_CAN_RESIZE: window can be resized.
 * @XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE: window can be placed above others.
 * @XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE: always above window can be
 *                                             returned to the normal stacking
 *                                             order.
 * @XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW: window can be placed below others.
 * @XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW: always below window can be
 *                                             returned to the normal stacking
 *                                             order.
 * @XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE: window can be moved to a
 *                                                different workspace or can be
 *                                                pinned and unpinned.
 *
 * Flags bitfield that describes actions that can be taken on the window.
 **/
typedef enum {
    XFW_WINDOW_CAPABILITIES_NONE = 0,
    XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE = (1 << 0),
    XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE = (1 << 1),
    XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE = (1 << 2),
    XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE = (1 << 3),
    XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN = (1 << 4),
    XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN = (1 << 5),
    XFW_WINDOW_CAPABILITIES_CAN_SHADE = (1 << 6),
    XFW_WINDOW_CAPABILITIES_CAN_UNSHADE = (1 << 7),
    XFW_WINDOW_CAPABILITIES_CAN_MOVE = (1 << 8),
    XFW_WINDOW_CAPABILITIES_CAN_RESIZE = (1 << 9),
    XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE = (1 << 10),
    XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE = (1 << 11),
    XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW = (1 << 12),
    XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW = (1 << 13),
    XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE = (1 << 14),
} XfwWindowCapabilities;

/**
 * XfwWindowType:
 * @XFW_WINDOW_TYPE_NORMAL: window is a regular window.
 * @XFW_WINDOW_TYPE_DESKTOP: window is responsible for drawing the desktop.
 * @XFW_WINDOW_TYPE_DOCK: window is a dock or panel.
 * @XFW_WINDOW_TYPE_DIALOG: window is a temporary dialog, like an error alert.
 * @XFW_WINDOW_TYPE_TOOLBAR: window is a detached toolbar.
 * @XFW_WINDOW_TYPE_MENU: window is a popup menu.
 * @XFW_WINDOW_TYPE_UTILITY: window is a utility menu, like a tool picker or
 *                           color palette.
 * @XFW_WINDOW_TYPE_SPLASHSCREEN: window is an application splash screen.
 *
 * Enumeration describing the windows type or function.
 **/
typedef enum {
    XFW_WINDOW_TYPE_NORMAL = 0,
    XFW_WINDOW_TYPE_DESKTOP = 1,
    XFW_WINDOW_TYPE_DOCK = 2,
    XFW_WINDOW_TYPE_DIALOG = 3,
    XFW_WINDOW_TYPE_TOOLBAR = 4,
    XFW_WINDOW_TYPE_MENU = 5,
    XFW_WINDOW_TYPE_UTILITY = 6,
    XFW_WINDOW_TYPE_SPLASHSCREEN = 7,
} XfwWindowType;

GType xfw_window_type_get_type(void);
GType xfw_window_state_get_type(void);
GType xfw_window_capabilities_get_type(void);

const gchar *const *xfw_window_get_class_ids(XfwWindow *window);
const gchar *xfw_window_get_name(XfwWindow *window);
GdkPixbuf *xfw_window_get_icon(XfwWindow *window, gint size, gint scale);
GIcon *xfw_window_get_gicon(XfwWindow *window);
gboolean xfw_window_icon_is_fallback(XfwWindow *window);
XfwWindowType xfw_window_get_window_type(XfwWindow *window);
XfwWindowState xfw_window_get_state(XfwWindow *window);
XfwWindowCapabilities xfw_window_get_capabilities(XfwWindow *window);
GdkRectangle *xfw_window_get_geometry(XfwWindow *window);
XfwScreen *xfw_window_get_screen(XfwWindow *window);
XfwWorkspace *xfw_window_get_workspace(XfwWindow *window);
GList *xfw_window_get_monitors(XfwWindow *window);
XfwApplication *xfw_window_get_application(XfwWindow *window);

gboolean xfw_window_activate(XfwWindow *window, XfwSeat *seat, guint64 event_timestamp, GError **error);
gboolean xfw_window_close(XfwWindow *window, guint64 event_timestamp, GError **error);
gboolean xfw_window_start_move(XfwWindow *window, GError **error);
gboolean xfw_window_start_resize(XfwWindow *window, GError **error);
gboolean xfw_window_set_geometry(XfwWindow *window, const GdkRectangle *rect, GError **error);
gboolean xfw_window_set_button_geometry(XfwWindow *window, GdkWindow *relative_to, const GdkRectangle *rect, GError **error);
gboolean xfw_window_move_to_workspace(XfwWindow *window, XfwWorkspace *workspace, GError **error);

gboolean xfw_window_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error);
gboolean xfw_window_set_maximized(XfwWindow *window, gboolean is_maximized, GError **error);
gboolean xfw_window_set_fullscreen(XfwWindow *window, gboolean is_fullscreen, GError **error);
gboolean xfw_window_set_skip_pager(XfwWindow *window, gboolean is_skip_pager, GError **error);
gboolean xfw_window_set_skip_tasklist(XfwWindow *window, gboolean is_skip_tasklist, GError **error);
gboolean xfw_window_set_pinned(XfwWindow *window, gboolean is_pinned, GError **error);
gboolean xfw_window_set_shaded(XfwWindow *window, gboolean is_shaded, GError **error);
gboolean xfw_window_set_above(XfwWindow *window, gboolean is_above, GError **error);
gboolean xfw_window_set_below(XfwWindow *window, gboolean is_below, GError **error);

gboolean xfw_window_is_active(XfwWindow *window);
gboolean xfw_window_is_minimized(XfwWindow *window);
gboolean xfw_window_is_maximized(XfwWindow *window);
gboolean xfw_window_is_fullscreen(XfwWindow *window);
gboolean xfw_window_is_skip_pager(XfwWindow *window);
gboolean xfw_window_is_skip_tasklist(XfwWindow *window);
gboolean xfw_window_is_pinned(XfwWindow *window);
gboolean xfw_window_is_shaded(XfwWindow *window);
gboolean xfw_window_is_above(XfwWindow *window);
gboolean xfw_window_is_below(XfwWindow *window);
gboolean xfw_window_is_urgent(XfwWindow *window);

gboolean xfw_window_is_on_workspace(XfwWindow *window, XfwWorkspace *workspace);
gboolean xfw_window_is_in_viewport(XfwWindow *window, XfwWorkspace *workspace);

G_END_DECLS

#endif /* !__XFW_WINDOW_H__ */
