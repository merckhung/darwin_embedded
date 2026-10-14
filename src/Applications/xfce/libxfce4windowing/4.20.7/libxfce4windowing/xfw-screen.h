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

#ifndef __XFW_SCREEN_H__
#define __XFW_SCREEN_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <libxfce4windowing/xfw-monitor.h>
#include <libxfce4windowing/xfw-window.h>
#include <libxfce4windowing/xfw-workspace-manager.h>

G_BEGIN_DECLS

#define XFW_TYPE_SCREEN (xfw_screen_get_type())
G_DECLARE_DERIVABLE_TYPE(XfwScreen, xfw_screen, XFW, SCREEN, GObject)

XfwScreen *xfw_screen_get_default(void);

GList *xfw_screen_get_seats(XfwScreen *screen);

XfwWorkspaceManager *xfw_screen_get_workspace_manager(XfwScreen *screen);

GList *xfw_screen_get_windows(XfwScreen *screen);
GList *xfw_screen_get_windows_stacked(XfwScreen *screen);
XfwWindow *xfw_screen_get_active_window(XfwScreen *screen);

GList *xfw_screen_get_monitors(XfwScreen *screen);
XfwMonitor *xfw_screen_get_primary_monitor(XfwScreen *screen);
XfwMonitor *xfw_screen_get_monitor_from_gdk_monitor(XfwScreen *screen,
                                                    GdkMonitor *monitor);

gboolean xfw_screen_get_show_desktop(XfwScreen *screen);
void xfw_screen_set_show_desktop(XfwScreen *screen, gboolean show);

G_END_DECLS

#endif /* !__XFW_SCREEN_H__ */
