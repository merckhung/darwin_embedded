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

#include <X11/X.h>
#include <gdk/gdkx.h>
#include <libwnck/libwnck.h>

#include "libxfce4windowing-private.h"
#include "xfw-monitor-x11.h"
#include "xfw-screen-private.h"
#include "xfw-screen-x11.h"
#include "xfw-util.h"
#include "xfw-window-x11.h"
#include "xfw-workspace-manager-x11.h"

struct _XfwScreenX11 {
    XfwScreen parent;

    WnckScreen *wnck_screen;
    GList *windows;
    GList *windows_stacked;
    GHashTable *wnck_windows;

    // _NET_WORKAREA is defined for each workspace
    GArray *workareas;  // GdkRectangle

    XfwMonitorManagerX11 *monitor_manager;
};

static void xfw_screen_x11_constructed(GObject *obj);
static void xfw_screen_x11_finalize(GObject *obj);
static GList *xfw_screen_x11_get_windows(XfwScreen *screen);
static GList *xfw_screen_x11_get_windows_stacked(XfwScreen *screen);
static void xfw_screen_x11_set_show_desktop(XfwScreen *screen, gboolean show);

static void window_opened(WnckScreen *wnck_screen, WnckWindow *window, XfwScreenX11 *screen);
static void window_closed(WnckScreen *wnck_screen, WnckWindow *window, XfwScreenX11 *screen);
static void active_window_changed(WnckScreen *wnck_screen, WnckWindow *previous_window, XfwScreenX11 *screen);
static void window_stacking_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen);
static void showing_desktop_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen);
static void window_manager_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen);
static void active_workspace_changed(WnckScreen *wnck_screen, WnckWorkspace *previous_workspace, XfwScreenX11 *screen);


G_DEFINE_FINAL_TYPE(XfwScreenX11, xfw_screen_x11, XFW_TYPE_SCREEN)


static void
xfw_screen_x11_class_init(XfwScreenX11Class *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    gklass->constructed = xfw_screen_x11_constructed;
    gklass->finalize = xfw_screen_x11_finalize;

    XfwScreenClass *screen_class = XFW_SCREEN_CLASS(klass);
    screen_class->get_windows = xfw_screen_x11_get_windows;
    screen_class->get_windows_stacked = xfw_screen_x11_get_windows_stacked;
    screen_class->set_show_desktop = xfw_screen_x11_set_show_desktop;
}

static void
xfw_screen_x11_init(XfwScreenX11 *screen) {}

static void
xfw_screen_x11_constructed(GObject *obj) {
    XfwScreen *screen = XFW_SCREEN(obj);
    XfwScreenX11 *xscreen = XFW_SCREEN_X11(obj);

    G_OBJECT_CLASS(xfw_screen_x11_parent_class)->constructed(obj);

    XfwSeat *default_seat = g_object_new(XFW_TYPE_SEAT,
                                         "name", "seat0",
                                         NULL);
    _xfw_screen_seat_added(screen, default_seat);

    _xfw_screen_set_workspace_manager(screen, _xfw_workspace_manager_x11_new(screen));

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    xscreen->wnck_screen = g_object_ref(wnck_screen_get(gdk_x11_screen_get_screen_number(_xfw_screen_get_gdk_screen(screen))));
    G_GNUC_END_IGNORE_DEPRECATIONS
    xscreen->wnck_windows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);

    for (GList *l = wnck_screen_get_windows(xscreen->wnck_screen); l != NULL; l = l->next) {
        XfwWindowX11 *window = g_object_new(XFW_TYPE_WINDOW_X11,
                                            "screen", screen,
                                            "wnck-window", l->data,
                                            NULL);
        xscreen->windows = g_list_prepend(xscreen->windows, window);
        g_hash_table_insert(xscreen->wnck_windows, l->data, window);
    }
    xscreen->windows = g_list_reverse(xscreen->windows);
    window_stacking_changed(xscreen->wnck_screen, xscreen);

    _xfw_screen_set_active_window(screen,
                                  g_hash_table_lookup(xscreen->wnck_windows,
                                                      wnck_screen_get_active_window(xscreen->wnck_screen)));

    g_signal_connect(xscreen->wnck_screen, "window-opened", G_CALLBACK(window_opened), xscreen);
    g_signal_connect(xscreen->wnck_screen, "window-closed", G_CALLBACK(window_closed), xscreen);
    g_signal_connect(xscreen->wnck_screen, "active-window-changed", G_CALLBACK(active_window_changed), xscreen);
    g_signal_connect(xscreen->wnck_screen, "window-stacking-changed", G_CALLBACK(window_stacking_changed), xscreen);
    g_signal_connect(xscreen->wnck_screen, "window-manager-changed", G_CALLBACK(window_manager_changed), xscreen);
    g_signal_connect(xscreen->wnck_screen, "showing-desktop-changed", G_CALLBACK(showing_desktop_changed), xscreen);
    g_signal_connect(xscreen->wnck_screen, "active-workspace-changed", G_CALLBACK(active_workspace_changed), xscreen);

    xscreen->monitor_manager = _xfw_monitor_manager_x11_new(xscreen);
}

static void
xfw_screen_x11_finalize(GObject *obj) {
    XfwScreenX11 *screen = XFW_SCREEN_X11(obj);

    _xfw_monitor_manager_x11_destroy(screen->monitor_manager);

    g_signal_handlers_disconnect_by_data(screen->wnck_screen, screen);
    g_list_free(screen->windows);
    g_list_free(screen->windows_stacked);
    g_hash_table_destroy(screen->wnck_windows);

    if (screen->workareas != NULL) {
        g_array_free(screen->workareas, TRUE);
    }

    // to be released last
    g_object_unref(screen->wnck_screen);

    G_OBJECT_CLASS(xfw_screen_x11_parent_class)->finalize(obj);
}

static GList *
xfw_screen_x11_get_windows(XfwScreen *screen) {
    return XFW_SCREEN_X11(screen)->windows;
}

static GList *
xfw_screen_x11_get_windows_stacked(XfwScreen *screen) {
    return XFW_SCREEN_X11(screen)->windows_stacked;
}

static void
xfw_screen_x11_set_show_desktop(XfwScreen *screen, gboolean show) {
    XfwScreenX11 *xscreen = XFW_SCREEN_X11(screen);
    if (!!show != wnck_screen_get_showing_desktop(xscreen->wnck_screen)) {
        wnck_screen_toggle_showing_desktop(xscreen->wnck_screen, show);
        _xfw_screen_set_show_desktop(screen, !!show);
    }
}

static void
window_opened(WnckScreen *wnck_screen, WnckWindow *wnck_window, XfwScreenX11 *screen) {
    XfwWindowX11 *window = XFW_WINDOW_X11(g_object_new(XFW_TYPE_WINDOW_X11,
                                                       "screen", screen,
                                                       "wnck-window", wnck_window,
                                                       NULL));
    screen->windows = g_list_prepend(screen->windows, window);
    g_hash_table_insert(screen->wnck_windows, wnck_window, window);
    // FIXME: window-stacking-changed signal will fire out of order
    window_stacking_changed(screen->wnck_screen, screen);
    g_signal_emit_by_name(screen, "window-opened", window);
}

static void
window_closed(WnckScreen *wnck_screen, WnckWindow *wnck_window, XfwScreenX11 *screen) {
    XfwWindowX11 *window = g_hash_table_lookup(screen->wnck_windows, wnck_window);
    if (window != NULL) {
        g_object_ref(window);

        g_hash_table_remove(screen->wnck_windows, wnck_window);
        screen->windows = g_list_remove(screen->windows, window);
        screen->windows_stacked = g_list_remove(screen->windows_stacked, window);

        if (xfw_screen_get_active_window(XFW_SCREEN(screen)) == XFW_WINDOW(window)) {
            _xfw_screen_set_active_window(XFW_SCREEN(screen), NULL);
        }

        g_signal_emit_by_name(window, "closed");
        g_signal_emit_by_name(screen, "window-closed", window);
        g_signal_emit_by_name(screen, "window-stacking-changed", screen);

        g_object_unref(window);
    }
}

static void
active_window_changed(WnckScreen *wnck_screen, WnckWindow *previous_wnck_window, XfwScreenX11 *screen) {
    WnckWindow *wnck_window = wnck_screen_get_active_window(screen->wnck_screen);
    XfwWindow *window = g_hash_table_lookup(screen->wnck_windows, wnck_window);
    if (window != xfw_screen_get_active_window(XFW_SCREEN(screen))) {
        if (previous_wnck_window != NULL) {
            g_signal_emit_by_name(previous_wnck_window, "state-changed", 0, wnck_window_get_state(previous_wnck_window));
        }
        if (wnck_window != NULL) {
            g_signal_emit_by_name(wnck_window, "state-changed", 0, wnck_window_get_state(wnck_window));
        }

        _xfw_screen_set_active_window(XFW_SCREEN(screen), window);
    }
}

static void
window_stacking_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen) {
    g_clear_list(&screen->windows_stacked, NULL);

    for (GList *l = wnck_screen_get_windows_stacked(screen->wnck_screen); l != NULL; l = l->next) {
        XfwWindowX11 *window = g_hash_table_lookup(screen->wnck_windows, l->data);
        if (window != NULL) {
            screen->windows_stacked = g_list_prepend(screen->windows_stacked, window);
        }
    }
    screen->windows_stacked = g_list_reverse(screen->windows_stacked);
    g_signal_emit_by_name(screen, "window-stacking-changed");
}

static void
window_manager_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen) {
    g_signal_emit_by_name(screen, "window-manager-changed");
}

static void
showing_desktop_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen) {
    gboolean show_desktop = wnck_screen_get_showing_desktop(wnck_screen);
    _xfw_screen_set_show_desktop(XFW_SCREEN(screen), show_desktop);
}

static void
active_workspace_changed(WnckScreen *wnck_screen, WnckWorkspace *previous_workspace, XfwScreenX11 *screen) {
    WnckWorkspace *cur_workspace = wnck_screen_get_active_workspace(screen->wnck_screen);
    gint cur_workspace_num = cur_workspace != NULL
                                 ? wnck_workspace_get_number(cur_workspace)
                                 : 0;
    _xfw_monitor_x11_workspace_changed(screen, cur_workspace_num);
}

XfwWorkspace *
_xfw_screen_x11_workspace_for_wnck_workspace(XfwScreenX11 *screen, WnckWorkspace *wnck_workspace) {
    XfwWorkspaceManager *workspace_manager = xfw_screen_get_workspace_manager(XFW_SCREEN(screen));
    return _xfw_workspace_manager_x11_workspace_for_wnck_workspace(XFW_WORKSPACE_MANAGER_X11(workspace_manager),
                                                                   wnck_workspace);
}

GArray *
_xfw_screen_x11_get_workareas(XfwScreenX11 *screen) {
    return screen->workareas;
}

void
_xfw_screen_x11_set_workareas(XfwScreenX11 *screen, GArray *workareas) {
    if (screen->workareas != NULL && screen->workareas != workareas) {
        g_array_free(screen->workareas, TRUE);
    }
    screen->workareas = workareas;
}
