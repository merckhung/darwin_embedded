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

#ifndef __XFW_WINDOW_PRIVATE_H__
#define __XFW_WINDOW_PRIVATE_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include "xfw-screen.h"
#include "xfw-window.h"

#define XFW_WINDOW_FALLBACK_ICON_NAME "window-maximize-symbolic"

G_BEGIN_DECLS

struct _XfwWindowClass {
    /*< private >*/
    GObjectClass parent_class;

    /*< public >*/

    /* Signals */
    void (*class_changed)(XfwWindow *window);
    void (*name_changed)(XfwWindow *window);
    void (*icon_changed)(XfwWindow *window);
    void (*type_changed)(XfwWindow *window, XfwWindowType old_type);
    void (*state_changed)(XfwWindow *window, XfwWindowState changed_mask, XfwWindowState new_state);
    void (*capabilities_changed)(XfwWindow *window, XfwWindowCapabilities changed_mask, XfwWindowCapabilities new_capabilities);
    void (*geometry_changed)(XfwWindow *window);
    void (*workspace_changed)(XfwWindow *window);
    void (*closed)(XfwWindow *window);

    /* Virtual Table */
    const gchar *const *(*get_class_ids)(XfwWindow *window);
    const gchar *(*get_name)(XfwWindow *window);
    GIcon *(*get_gicon)(XfwWindow *window);
    XfwWindowType (*get_window_type)(XfwWindow *window);
    XfwWindowState (*get_state)(XfwWindow *window);
    XfwWindowCapabilities (*get_capabilities)(XfwWindow *window);
    GdkRectangle *(*get_geometry)(XfwWindow *window);
    XfwScreen *(*get_screen)(XfwWindow *window);
    XfwWorkspace *(*get_workspace)(XfwWindow *window);
    GList *(*get_monitors)(XfwWindow *window);
    XfwApplication *(*get_application)(XfwWindow *window);

    gboolean (*activate)(XfwWindow *window, XfwSeat *seat, guint64 event_timestamp, GError **error);
    gboolean (*close)(XfwWindow *window, guint64 event_timestamp, GError **error);
    gboolean (*start_move)(XfwWindow *window, GError **error);
    gboolean (*start_resize)(XfwWindow *window, GError **error);
    gboolean (*set_geometry)(XfwWindow *window, const GdkRectangle *rect, GError **error);
    gboolean (*set_button_geometry)(XfwWindow *window, GdkWindow *relative_to, const GdkRectangle *rect, GError **error);
    gboolean (*move_to_workspace)(XfwWindow *window, XfwWorkspace *workspace, GError **error);

    gboolean (*set_minimized)(XfwWindow *window, gboolean is_minimized, GError **error);
    gboolean (*set_maximized)(XfwWindow *window, gboolean is_maximized, GError **error);
    gboolean (*set_fullscreen)(XfwWindow *window, gboolean is_fullscreen, GError **error);
    gboolean (*set_skip_pager)(XfwWindow *window, gboolean is_skip_pager, GError **error);
    gboolean (*set_skip_tasklist)(XfwWindow *window, gboolean is_skip_tasklist, GError **error);
    gboolean (*set_pinned)(XfwWindow *window, gboolean is_pinned, GError **error);
    gboolean (*set_shaded)(XfwWindow *window, gboolean is_shaded, GError **error);
    gboolean (*set_above)(XfwWindow *window, gboolean is_above, GError **error);
    gboolean (*set_below)(XfwWindow *window, gboolean is_below, GError **error);

    gboolean (*is_on_workspace)(XfwWindow *window, XfwWorkspace *workspace);
    gboolean (*is_in_viewport)(XfwWindow *window, XfwWorkspace *workspace);
};

XfwScreen *_xfw_window_get_screen(XfwWindow *window);
void _xfw_window_invalidate_icon(XfwWindow *window);

G_END_DECLS

#endif /* !__XFW_WINDOW_PRIVATE_H__ */
