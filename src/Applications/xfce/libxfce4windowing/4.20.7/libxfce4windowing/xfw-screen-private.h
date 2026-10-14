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

#ifndef __XFW_SCREEN_PRIVATE_H__
#define __XFW_SCREEN_PRIVATE_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <gdk/gdk.h>

#include "xfw-screen.h"
#include "xfw-seat.h"
#include "xfw-window.h"
#include "xfw-workspace-manager.h"

G_BEGIN_DECLS

struct _XfwScreenClass {
    /*< private >*/
    GObjectClass parent_class;

    /*< public >*/

    /* Signals */
    void (*window_opened)(XfwScreen *screen, XfwWindow *window);
    void (*active_window_changed)(XfwScreen *screen, XfwWindow *previous_active_window);
    void (*window_stacking_changed)(XfwScreen *screen);
    void (*window_closed)(XfwScreen *screen, XfwWindow *window);
    void (*window_manager_changed)(XfwScreen *screen);

    /* Virtual Table */
    GList *(*get_windows)(XfwScreen *screen);
    GList *(*get_windows_stacked)(XfwScreen *screen);

    void (*set_show_desktop)(XfwScreen *screen, gboolean show);
};

GdkScreen *_xfw_screen_get_gdk_screen(XfwScreen *screen);

void _xfw_screen_seat_added(XfwScreen *screen, XfwSeat *seat);
void _xfw_screen_seat_removed(XfwScreen *screen, XfwSeat *seat);

void _xfw_screen_set_workspace_manager(XfwScreen *screen, XfwWorkspaceManager *workspace_manager);

void _xfw_screen_set_active_window(XfwScreen *screen, XfwWindow *window);

GList *_xfw_screen_steal_monitors(XfwScreen *screen);
void _xfw_screen_set_monitors(XfwScreen *screen, GList *monitors, GList *added, GList *removed);
void _xfw_screen_set_primary_monitor(XfwScreen *screen, XfwMonitor *primary_monitor);

void _xfw_screen_set_show_desktop(XfwScreen *screen, gboolean show_desktop);

G_END_DECLS

#endif /* !__XFW_SCREEN_PRIVATE_H__ */
