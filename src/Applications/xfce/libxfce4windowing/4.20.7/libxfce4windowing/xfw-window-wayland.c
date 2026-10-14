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

#include "protocols/ext-workspace-v1-client.h"
#include "protocols/wlr-foreign-toplevel-management-unstable-v1-client.h"
#include "protocols/xfce-foreign-toplevel-management-private-v1-client.h"

#include "libxfce4windowing-private.h"
#include "xfw-application-wayland.h"
#include "xfw-monitor-wayland.h"
#include "xfw-screen-wayland.h"
#include "xfw-screen.h"
#include "xfw-seat-wayland.h"
#include "xfw-util.h"
#include "xfw-window-private.h"
#include "xfw-window-wayland.h"
#include "xfw-wl-raster-icon.h"
#include "xfw-workspace-manager-wayland.h"
#include "xfw-workspace-wayland.h"

enum {
    PROP_0,
    PROP_WLR_HANDLE,
    PROP_XFCE_HANDLE,
};

typedef struct _PendingChanges {
    gchar *new_app_id;
    gchar *new_name;

    gboolean wlr_state_changed;
    XfwWindowState new_wlr_state;
    gboolean xfce_state_changed;
    XfwWindowState new_xfce_state;

    GList *monitors_to_add;
    GList *monitors_to_remove;

    gboolean workspace_changed;
    XfwWorkspace *new_workspace;

    gboolean icon_changed;
    gchar *new_icon_name;
    GList *new_icon_sizes;
} PendingChanges;

struct _XfwWindowWaylandPrivate {
    struct zwlr_foreign_toplevel_handle_v1 *wlr_handle;
    struct xfce_foreign_toplevel_handle_v1 *xfce_handle;
    gint initial_dones_seen;
    gboolean created_emitted;

    PendingChanges pending;

    const gchar **class_ids;
    gchar *app_id;
    gchar *name;
    XfwWindowState state;
    XfwWindowCapabilities capabilities;
    GdkRectangle geometry;  // unfortunately unsupported
    GList *monitors;
    GList *pending_outputs;
    guint pending_outputs_id;
    XfwApplication *app;
    XfwWorkspace *workspace;
    gchar *icon_name;
    GList *icon_sizes;  // IconSize
    GIcon *icon;
};

static void xfw_window_wayland_constructed(GObject *obj);
static void xfw_window_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_window_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_window_wayland_finalize(GObject *obj);

static const gchar *const *xfw_window_wayland_get_class_ids(XfwWindow *window);
static const gchar *xfw_window_wayland_get_name(XfwWindow *window);
static GIcon *xfw_window_wayland_get_gicon(XfwWindow *window);
static XfwWindowType xfw_window_wayland_get_window_type(XfwWindow *window);
static XfwWindowState xfw_window_wayland_get_state(XfwWindow *window);
static XfwWindowCapabilities xfw_window_wayland_get_capabilities(XfwWindow *window);
static GdkRectangle *xfw_window_wayland_get_geometry(XfwWindow *window);
static XfwWorkspace *xfw_window_wayland_get_workspace(XfwWindow *window);
static GList *xfw_window_wayland_get_monitors(XfwWindow *window);
static XfwApplication *xfw_window_wayland_get_application(XfwWindow *window);
static gboolean xfw_window_wayland_activate(XfwWindow *window, XfwSeat *seat, guint64 event_timestamp, GError **error);
static gboolean xfw_window_wayland_close(XfwWindow *window, guint64 event_timestamp, GError **error);
static gboolean xfw_window_wayland_start_move(XfwWindow *window, GError **error);
static gboolean xfw_window_wayland_start_resize(XfwWindow *window, GError **error);
static gboolean xfw_window_wayland_set_geometry(XfwWindow *window, const GdkRectangle *rect, GError **error);
static gboolean xfw_window_wayland_set_button_geometry(XfwWindow *window, GdkWindow *relative_to, const GdkRectangle *rect, GError **error);
static gboolean xfw_window_wayland_move_to_workspace(XfwWindow *window, XfwWorkspace *workspace, GError **error);
static gboolean xfw_window_wayland_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error);
static gboolean xfw_window_wayland_set_maximized(XfwWindow *window, gboolean is_maximized, GError **error);
static gboolean xfw_window_wayland_set_fullscreen(XfwWindow *window, gboolean is_fullscreen, GError **error);
static gboolean xfw_window_wayland_set_skip_pager(XfwWindow *window, gboolean is_skip_pager, GError **error);
static gboolean xfw_window_wayland_set_skip_tasklist(XfwWindow *window, gboolean is_skip_tasklist, GError **error);
static gboolean xfw_window_wayland_set_pinned(XfwWindow *window, gboolean is_pinned, GError **error);
static gboolean xfw_window_wayland_set_shaded(XfwWindow *window, gboolean is_pinned, GError **error);
static gboolean xfw_window_wayland_set_above(XfwWindow *window, gboolean is_above, GError **error);
static gboolean xfw_window_wayland_set_below(XfwWindow *window, gboolean is_below, GError **error);
static gboolean xfw_window_wayland_is_on_workspace(XfwWindow *window, XfwWorkspace *workspace);
static gboolean xfw_window_wayland_is_in_viewport(XfwWindow *window, XfwWorkspace *workspace);

static void wlr_toplevel_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, const char *app_id);
static void wlr_toplevel_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, const char *title);
static void wlr_toplevel_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct wl_array *wl_state);
static void wlr_toplevel_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct zwlr_foreign_toplevel_handle_v1 *wl_parent);
static void wlr_toplevel_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct wl_output *output);
static void wlr_toplevel_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct wl_output *output);
static void wlr_toplevel_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel);
static void wlr_toplevel_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel);

static void xfce_toplevel_state(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, struct wl_array *xfce_state);
static void xfce_toplevel_icon_name(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, const char *name);
static void xfce_toplevel_icon_size(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, uint32_t size, uint32_t scale);
static void xfce_toplevel_no_icon(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel);
static void xfce_toplevel_workspace_enter(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, struct ext_workspace_handle_v1 *ext_workspace);
static void xfce_toplevel_workspace_leave(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, struct ext_workspace_handle_v1 *ext_workspace);

static void monitor_added(XfwScreen *screen, XfwMonitor *monitor, XfwWindowWayland *window);
static void monitor_removed(XfwScreen *screen, XfwMonitor *monitor, XfwWindowWayland *window);

static IconSize *icon_size_new(uint32_t size, uint32_t scale);

static const struct {
    enum zwlr_foreign_toplevel_handle_v1_state proto_state;
    XfwWindowState state_bit;
} wlr_state_converters[] = {
    { ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED, XFW_WINDOW_STATE_ACTIVE },
    { ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED, XFW_WINDOW_STATE_MINIMIZED },
    { ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED, XFW_WINDOW_STATE_MAXIMIZED },
    { ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN, XFW_WINDOW_STATE_FULLSCREEN },
};
static const XfwWindowState wlr_window_state_mask = XFW_WINDOW_STATE_ACTIVE | XFW_WINDOW_STATE_MINIMIZED | XFW_WINDOW_STATE_MAXIMIZED | XFW_WINDOW_STATE_FULLSCREEN;

static const struct {
    enum xfce_foreign_toplevel_handle_v1_state proto_state;
    XfwWindowState state_bit;
} xfce_state_converters[] = {
    { XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_SHADED, XFW_WINDOW_STATE_SHADED },
    { XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_SKIP_PAGER, XFW_WINDOW_STATE_SKIP_PAGER },
    { XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_SKIP_TASKLIST, XFW_WINDOW_STATE_SKIP_TASKLIST },
    { XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_STICKY, XFW_WINDOW_STATE_PINNED },
    { XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ABOVE, XFW_WINDOW_STATE_ABOVE },
    { XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_BELOW, XFW_WINDOW_STATE_BELOW },
    { XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_DEMANDS_ATTENTION, XFW_WINDOW_STATE_URGENT },
};
static const XfwWindowState xfce_window_state_mask = XFW_WINDOW_STATE_SHADED | XFW_WINDOW_STATE_SKIP_PAGER | XFW_WINDOW_STATE_SKIP_TASKLIST | XFW_WINDOW_STATE_PINNED | XFW_WINDOW_STATE_ABOVE | XFW_WINDOW_STATE_BELOW | XFW_WINDOW_STATE_URGENT;

static const struct {
    XfwWindowState state_bit;
    XfwWindowCapabilities capabilities_bit_if_present;
    XfwWindowCapabilities capabilities_bit_if_absent;
} wlr_capabilities_converters[] = {
    { XFW_WINDOW_STATE_MINIMIZED, XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE, XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE },
    { XFW_WINDOW_STATE_MAXIMIZED, XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE, XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE | XFW_WINDOW_CAPABILITIES_CAN_RESIZE },
    { XFW_WINDOW_STATE_FULLSCREEN, XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN, XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN },
};

static const struct {
    XfwWindowState state_bit;
    XfwWindowCapabilities capabilities_bit_if_present;
    XfwWindowCapabilities capabilities_bit_if_absent;
} xfce_capabilities_converters[] = {
    { XFW_WINDOW_STATE_SHADED, XFW_WINDOW_CAPABILITIES_CAN_UNSHADE, XFW_WINDOW_CAPABILITIES_CAN_SHADE },
    { XFW_WINDOW_STATE_PINNED, 0, XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE },
    { XFW_WINDOW_STATE_ABOVE, XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE, XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE },
    { XFW_WINDOW_STATE_BELOW, XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW, XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW },
};

static const struct zwlr_foreign_toplevel_handle_v1_listener wlr_toplevel_handle_listener = {
    .app_id = wlr_toplevel_app_id,
    .title = wlr_toplevel_title,
    .state = wlr_toplevel_state,
    .parent = wlr_toplevel_parent,
    .output_enter = wlr_toplevel_output_enter,
    .output_leave = wlr_toplevel_output_leave,
    .closed = wlr_toplevel_closed,
    .done = wlr_toplevel_done,
};

static const struct xfce_foreign_toplevel_handle_v1_listener xfce_toplevel_handle_listener = {
    .state = xfce_toplevel_state,
    .icon_name = xfce_toplevel_icon_name,
    .icon_size = xfce_toplevel_icon_size,
    .no_icon = xfce_toplevel_no_icon,
    .workspace_enter = xfce_toplevel_workspace_enter,
    .workspace_leave = xfce_toplevel_workspace_leave,
};

G_DEFINE_FINAL_TYPE_WITH_PRIVATE(XfwWindowWayland, xfw_window_wayland, XFW_TYPE_WINDOW)

static void
xfw_window_wayland_class_init(XfwWindowWaylandClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    XfwWindowClass *window_class = XFW_WINDOW_CLASS(klass);

    gklass->constructed = xfw_window_wayland_constructed;
    gklass->set_property = xfw_window_wayland_set_property;
    gklass->get_property = xfw_window_wayland_get_property;
    gklass->finalize = xfw_window_wayland_finalize;

    window_class->get_class_ids = xfw_window_wayland_get_class_ids;
    window_class->get_name = xfw_window_wayland_get_name;
    window_class->get_gicon = xfw_window_wayland_get_gicon;
    window_class->get_window_type = xfw_window_wayland_get_window_type;
    window_class->get_state = xfw_window_wayland_get_state;
    window_class->get_capabilities = xfw_window_wayland_get_capabilities;
    window_class->get_geometry = xfw_window_wayland_get_geometry;
    window_class->get_workspace = xfw_window_wayland_get_workspace;
    window_class->get_monitors = xfw_window_wayland_get_monitors;
    window_class->get_application = xfw_window_wayland_get_application;
    window_class->activate = xfw_window_wayland_activate;
    window_class->close = xfw_window_wayland_close;
    window_class->start_move = xfw_window_wayland_start_move;
    window_class->start_resize = xfw_window_wayland_start_resize;
    window_class->set_geometry = xfw_window_wayland_set_geometry;
    window_class->set_button_geometry = xfw_window_wayland_set_button_geometry;
    window_class->move_to_workspace = xfw_window_wayland_move_to_workspace;
    window_class->set_minimized = xfw_window_wayland_set_minimized;
    window_class->set_maximized = xfw_window_wayland_set_maximized;
    window_class->set_fullscreen = xfw_window_wayland_set_fullscreen;
    window_class->set_skip_pager = xfw_window_wayland_set_skip_pager;
    window_class->set_skip_tasklist = xfw_window_wayland_set_skip_tasklist;
    window_class->set_pinned = xfw_window_wayland_set_pinned;
    window_class->set_shaded = xfw_window_wayland_set_shaded;
    window_class->set_above = xfw_window_wayland_set_above;
    window_class->set_below = xfw_window_wayland_set_below;
    window_class->is_on_workspace = xfw_window_wayland_is_on_workspace;
    window_class->is_in_viewport = xfw_window_wayland_is_in_viewport;

    g_object_class_install_property(gklass,
                                    PROP_WLR_HANDLE,
                                    g_param_spec_pointer("wlr-handle",
                                                         "wlr-handle",
                                                         "wlr-handle",
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(gklass,
                                    PROP_XFCE_HANDLE,
                                    g_param_spec_pointer("xfce-handle",
                                                         "xfce-handle",
                                                         "xfce-handle",
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_window_wayland_init(XfwWindowWayland *window) {
    window->priv = xfw_window_wayland_get_instance_private(window);
    window->priv->class_ids = g_new0(const gchar *, 2);
    window->priv->capabilities = XFW_WINDOW_CAPABILITIES_CAN_MOVE | XFW_WINDOW_CAPABILITIES_CAN_RESIZE;
}

static void
xfw_window_wayland_constructed(GObject *obj) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(obj);
    zwlr_foreign_toplevel_handle_v1_add_listener(window->priv->wlr_handle, &wlr_toplevel_handle_listener, window);
    if (window->priv->xfce_handle != NULL) {
        xfce_foreign_toplevel_handle_v1_add_listener(window->priv->xfce_handle, &xfce_toplevel_handle_listener, window);
    }

    XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
    g_signal_connect(screen, "monitor-added", G_CALLBACK(monitor_added), window);
    g_signal_connect(screen, "monitor-removed", G_CALLBACK(monitor_removed), window);

    G_OBJECT_CLASS(xfw_window_wayland_parent_class)->constructed(obj);
}

static void
xfw_window_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(obj);

    switch (prop_id) {
        case PROP_WLR_HANDLE:
            window->priv->wlr_handle = g_value_get_pointer(value);
            break;

        case PROP_XFCE_HANDLE:
            window->priv->xfce_handle = g_value_get_pointer(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_window_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWindow *window = XFW_WINDOW(obj);

    switch (prop_id) {
        case PROP_WLR_HANDLE:
            g_value_set_pointer(value, XFW_WINDOW_WAYLAND(window)->priv->wlr_handle);
            break;

        case PROP_XFCE_HANDLE:
            g_value_set_pointer(value, XFW_WINDOW_WAYLAND(window)->priv->xfce_handle);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_window_wayland_finalize(GObject *obj) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(obj);

    g_signal_handlers_disconnect_by_data(_xfw_window_get_screen(XFW_WINDOW(window)), window);

    if (window->priv->xfce_handle != NULL) {
        xfce_foreign_toplevel_handle_v1_destroy(window->priv->xfce_handle);
    }
    zwlr_foreign_toplevel_handle_v1_destroy(window->priv->wlr_handle);
    g_free(window->priv->class_ids);
    g_free(window->priv->app_id);
    g_free(window->priv->name);
    g_list_free(window->priv->monitors);
    g_list_free(window->priv->pending_outputs);
    if (window->priv->pending_outputs_id != 0) {
        g_source_remove(window->priv->pending_outputs_id);
    }
    g_object_unref(window->priv->app);
    g_free(window->priv->icon_name);
    g_list_free_full(window->priv->icon_sizes, g_free);
    if (window->priv->icon != NULL) {
        g_object_unref(window->priv->icon);
    }

    // TODO: free pending

    G_OBJECT_CLASS(xfw_window_wayland_parent_class)->finalize(obj);
}

static const gchar *const *
xfw_window_wayland_get_class_ids(XfwWindow *window) {
    return XFW_WINDOW_WAYLAND(window)->priv->class_ids;
}

static const gchar *
xfw_window_wayland_get_name(XfwWindow *window) {
    return XFW_WINDOW_WAYLAND(window)->priv->name;
}

static GIcon *
xfw_window_wayland_get_gicon(XfwWindow *window) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);

    if (wwindow->priv->icon != NULL) {
        return g_object_ref(wwindow->priv->icon);
    } else {
        GIcon *gicon = _xfw_application_wayland_get_gicon_no_fallback(XFW_APPLICATION_WAYLAND(wwindow->priv->app));

        if (gicon != NULL) {
            return gicon;
        } else {
            return g_themed_icon_new_with_default_fallbacks(XFW_WINDOW_FALLBACK_ICON_NAME);
        }
    }
}

static XfwWindowType
xfw_window_wayland_get_window_type(XfwWindow *window) {
    _xfw_g_message_once("Window types are not supported on Wayland");
    return XFW_WINDOW_TYPE_NORMAL;
}

static XfwWindowState
xfw_window_wayland_get_state(XfwWindow *window) {
    return XFW_WINDOW_WAYLAND(window)->priv->state;
}

static XfwWindowCapabilities
xfw_window_wayland_get_capabilities(XfwWindow *window) {
    return XFW_WINDOW_WAYLAND(window)->priv->capabilities;
}

static GdkRectangle *
xfw_window_wayland_get_geometry(XfwWindow *window) {
    _xfw_g_message_once("xfw_window_get_geometry() unsupported on Wayland");
    return &XFW_WINDOW_WAYLAND(window)->priv->geometry;
}

static XfwWorkspace *
xfw_window_wayland_get_workspace(XfwWindow *window) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        return wwindow->priv->workspace;
    } else {
        XfwScreen *screen = _xfw_window_get_screen(window);
        return _xfw_screen_wayland_get_window_workspace(XFW_SCREEN_WAYLAND(screen), window);
    }
}

static GList *
xfw_window_wayland_get_monitors(XfwWindow *window) {
    return XFW_WINDOW_WAYLAND(window)->priv->monitors;
}

static XfwApplication *
xfw_window_wayland_get_application(XfwWindow *window) {
    return XFW_WINDOW_WAYLAND(window)->priv->app;
}

static gboolean
xfw_window_wayland_activate(XfwWindow *window, XfwSeat *seat, guint64 event_timestamp, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);

    GList *seats = NULL;
    if (seat != NULL) {
        seats = g_list_prepend(seats, seat);
    } else {
        XfwScreen *screen = _xfw_window_get_screen(window);
        seats = g_list_copy(xfw_screen_get_seats(screen));
    }

    if (seats == NULL) {
        if (error != NULL) {
            *error = g_error_new(XFW_ERROR, XFW_ERROR_INTERNAL, "Cannot activate window as we do not have a wl_seat");
        }
        return FALSE;
    } else {
        for (GList *l = seats; l != NULL; l = l->next) {
            XfwSeatWayland *a_seat = XFW_SEAT_WAYLAND(l->data);
            struct wl_seat *wl_seat = _xfw_seat_wayland_get_wl_seat(a_seat);
            zwlr_foreign_toplevel_handle_v1_activate(wwindow->priv->wlr_handle, wl_seat);
        }
        g_list_free(seats);
        return TRUE;
    }
}

static gboolean
xfw_window_wayland_close(XfwWindow *window, guint64 event_timestamp, GError **error) {
    zwlr_foreign_toplevel_handle_v1_close(XFW_WINDOW_WAYLAND(window)->priv->wlr_handle);
    return TRUE;
}

static gboolean
xfw_window_wayland_start_move(XfwWindow *window, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_MOVE) != 0) {
            // This function should really take a XfwSeat argument, but too late to change it now.
            XfwScreen *screen = _xfw_window_get_screen(window);
            GList *seats = xfw_screen_get_seats(screen);
            if (seats != NULL) {
                XfwSeatWayland *seat = XFW_SEAT_WAYLAND(seats->data);
                xfce_foreign_toplevel_handle_v1_move(wwindow->priv->xfce_handle, _xfw_seat_wayland_get_wl_seat(seat));
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_INTERNAL, "Cannot move window as we do not have a wl_seat");
                return FALSE;
            }
        } else {
            g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being moved");
            return FALSE;
        }
    } else {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Moving windows is not supported on your compositor");
        return FALSE;
    }
}

static gboolean
xfw_window_wayland_start_resize(XfwWindow *window, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_RESIZE) != 0) {
            // This function should really take a XfwSeat argument, but too late to change it now.
            XfwScreen *screen = _xfw_window_get_screen(window);
            GList *seats = xfw_screen_get_seats(screen);
            if (seats != NULL) {
                XfwSeatWayland *seat = XFW_SEAT_WAYLAND(seats->data);
                xfce_foreign_toplevel_handle_v1_resize(wwindow->priv->xfce_handle, _xfw_seat_wayland_get_wl_seat(seat));
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_INTERNAL, "Cannot resize window as we do not have a wl_seat");
                return FALSE;
            }
        } else {
            g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being resized");
            return FALSE;
        }
    } else {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Resizing windows is not supported on your compositor");
        return FALSE;
    }
}

static gboolean
xfw_window_wayland_set_geometry(XfwWindow *window, const GdkRectangle *rect, GError **error) {
    if (error != NULL) {
        *error = g_error_new(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Setting windows geometry is not supported on Wayland");
    }
    return FALSE;
}

static gboolean
xfw_window_wayland_set_button_geometry(XfwWindow *window, GdkWindow *relative_to, const GdkRectangle *rect, GError **error) {
    zwlr_foreign_toplevel_handle_v1_set_rectangle(XFW_WINDOW_WAYLAND(window)->priv->wlr_handle, gdk_wayland_window_get_wl_surface(relative_to), rect->x, rect->y, rect->width, rect->height);
    return TRUE;
}

static gboolean
xfw_window_wayland_move_to_workspace(XfwWindow *window, XfwWorkspace *workspace, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0) {
            if (XFW_IS_WORKSPACE_WAYLAND(workspace)) {
                xfce_foreign_toplevel_handle_v1_move_to_workspace(wwindow->priv->xfce_handle, _xfw_workspace_wayland_get_handle(XFW_WORKSPACE_WAYLAND(workspace)));
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Moving windows between workspaces is not supported on your compositor");
                return FALSE;
            }
        } else {
            g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being moved between workspaces");
            return FALSE;
        }
    } else {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Moving windows between workspaces is not supported on your compositor");
        return FALSE;
    }
}

static gboolean
xfw_window_wayland_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);

    if (is_minimized) {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE) != 0) {
            zwlr_foreign_toplevel_handle_v1_set_minimized(wwindow->priv->wlr_handle);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being minimized");
            }
            return FALSE;
        }
    } else {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE) != 0) {
            zwlr_foreign_toplevel_handle_v1_unset_minimized(wwindow->priv->wlr_handle);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being unminimized");
            }
            return FALSE;
        }
    }
}

static gboolean
xfw_window_wayland_set_maximized(XfwWindow *window, gboolean is_maximized, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);

    if (is_maximized) {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE) != 0) {
            zwlr_foreign_toplevel_handle_v1_set_maximized(wwindow->priv->wlr_handle);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being maximized");
            }
            return FALSE;
        }
    } else {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE) != 0) {
            zwlr_foreign_toplevel_handle_v1_unset_maximized(wwindow->priv->wlr_handle);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being unmaximized");
            }
            return FALSE;
        }
    }
}

static gboolean
xfw_window_wayland_set_fullscreen(XfwWindow *window, gboolean is_fullscreen, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);

    if (is_fullscreen) {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN) != 0) {
            zwlr_foreign_toplevel_handle_v1_set_fullscreen(wwindow->priv->wlr_handle, NULL);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being set fullscreen");
            }
            return FALSE;
        }
    } else {
        if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN) != 0) {
            zwlr_foreign_toplevel_handle_v1_unset_fullscreen(wwindow->priv->wlr_handle);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being unset fullscreen");
            }
            return FALSE;
        }
    }
}

static gboolean
xfw_window_wayland_set_skip_pager(XfwWindow *window, gboolean is_skip_pager, GError **error) {
    g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Changing the skip-pager state is not supported on Wayland");
    return FALSE;
}

static gboolean
xfw_window_wayland_set_skip_tasklist(XfwWindow *window, gboolean is_skip_tasklist, GError **error) {
    g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Changing the skip-tasklist state is not supported on Wayland");
    return FALSE;
}

static gboolean
xfw_window_wayland_set_pinned(XfwWindow *window, gboolean is_pinned, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        if (is_pinned) {
            xfce_foreign_toplevel_handle_v1_set_sticky(wwindow->priv->xfce_handle);
        } else {
            xfce_foreign_toplevel_handle_v1_unset_sticky(wwindow->priv->xfce_handle);
        }
        return TRUE;
    } else {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Window pinning is not supported on your compositor");
        return FALSE;
    }
}

static gboolean
xfw_window_wayland_set_shaded(XfwWindow *window, gboolean is_shaded, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        if (is_shaded) {
            if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_SHADE) != 0) {
                xfce_foreign_toplevel_handle_v1_set_shaded(wwindow->priv->xfce_handle);
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being shaded");
                return FALSE;
            }
        } else {
            if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNSHADE) != 0) {
                xfce_foreign_toplevel_handle_v1_unset_shaded(wwindow->priv->xfce_handle);
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being unshaded");
                return FALSE;
            }
        }
    } else {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Window shading is not supported on your compositor");
        return FALSE;
    }
}

static gboolean
xfw_window_wayland_set_above(XfwWindow *window, gboolean is_above, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        if (is_above) {
            if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE) != 0) {
                xfce_foreign_toplevel_handle_v1_set_above(wwindow->priv->xfce_handle);
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed above other windows");
                return FALSE;
            }
        } else {
            if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE) != 0) {
                xfce_foreign_toplevel_handle_v1_unset_above(wwindow->priv->xfce_handle);
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed back in the normal stacking order");
                return FALSE;
            }
        }
    } else {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Placing windows above others is not supported on your compositor");
        return FALSE;
    }
}

static gboolean
xfw_window_wayland_set_below(XfwWindow *window, gboolean is_below, GError **error) {
    XfwWindowWayland *wwindow = XFW_WINDOW_WAYLAND(window);
    if (wwindow->priv->xfce_handle != NULL) {
        if (is_below) {
            if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW) != 0) {
                xfce_foreign_toplevel_handle_v1_set_below(wwindow->priv->xfce_handle);
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed below other windows");
                return FALSE;
            }
        } else {
            if ((wwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW) != 0) {
                xfce_foreign_toplevel_handle_v1_unset_below(wwindow->priv->xfce_handle);
                return TRUE;
            } else {
                g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed back in the normal stacking order");
                return FALSE;
            }
        }
    } else {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_UNSUPPORTED, "Placing windows below others is not supported on your compositor");
        return FALSE;
    }
}

static gboolean
xfw_window_wayland_is_on_workspace(XfwWindow *window, XfwWorkspace *workspace) {
    return xfw_window_wayland_get_workspace(window) == workspace;
}

static gboolean
xfw_window_wayland_is_in_viewport(XfwWindow *window, XfwWorkspace *workspace) {
    return FALSE;
}

static void
xfw_window_commit_changes(XfwWindowWayland *window) {
    PendingChanges *pending = &window->priv->pending;

    if (pending->new_app_id != NULL) {
        _xfw_window_invalidate_icon(XFW_WINDOW(window));

        g_free(window->priv->app_id);
        window->priv->app_id = pending->new_app_id;
        window->priv->class_ids[0] = window->priv->app_id;

        if (window->priv->app != NULL) {
            g_object_unref(window->priv->app);
        }
        window->priv->app = XFW_APPLICATION(_xfw_application_wayland_get(window, window->priv->app_id));
    }

    if (pending->new_name != NULL) {
        g_free(window->priv->name);
        window->priv->name = pending->new_name;
    }

    XfwWindowState old_state = window->priv->state;
    if (pending->wlr_state_changed) {
        window->priv->state = (window->priv->state & ~wlr_window_state_mask) | pending->new_wlr_state;
    }
    if (pending->xfce_state_changed) {
        window->priv->state = (window->priv->state & ~xfce_window_state_mask) | pending->new_xfce_state;
    }

    XfwWindowState state_changed_mask;
    XfwWindowCapabilities capabilities_changed_mask;
    if (pending->wlr_state_changed || pending->xfce_state_changed) {
        state_changed_mask = old_state ^ window->priv->state;

        // Can clear these up here as below we use state_changed_mask to determine if the state changed
        pending->wlr_state_changed = FALSE;
        pending->new_wlr_state = 0;
        pending->xfce_state_changed = FALSE;
        pending->new_xfce_state = 0;

        XfwWindowCapabilities new_capabilities = XFW_WINDOW_CAPABILITIES_CAN_MOVE;
        for (size_t i = 0; i < G_N_ELEMENTS(wlr_capabilities_converters); ++i) {
            if ((window->priv->state & wlr_capabilities_converters[i].state_bit) != 0) {
                new_capabilities |= wlr_capabilities_converters[i].capabilities_bit_if_present;
            } else {
                new_capabilities |= wlr_capabilities_converters[i].capabilities_bit_if_absent;
            }
        }
        if (window->priv->xfce_handle != NULL) {
            for (size_t i = 0; i < G_N_ELEMENTS(xfce_capabilities_converters); ++i) {
                if ((window->priv->state & xfce_capabilities_converters[i].state_bit) != 0) {
                    new_capabilities |= xfce_capabilities_converters[i].capabilities_bit_if_present;
                } else {
                    new_capabilities |= xfce_capabilities_converters[i].capabilities_bit_if_absent;
                }
            }
        }

        if ((window->priv->state & XFW_WINDOW_STATE_MINIMIZED) != 0) {
            new_capabilities &= ~(XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN
                                  | XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE
                                  | XFW_WINDOW_CAPABILITIES_CAN_SHADE
                                  | XFW_WINDOW_CAPABILITIES_CAN_RESIZE
                                  | XFW_WINDOW_CAPABILITIES_CAN_MOVE);
        } else if ((window->priv->state & XFW_WINDOW_STATE_FULLSCREEN) != 0) {
            new_capabilities &= ~(XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE
                                  | XFW_WINDOW_CAPABILITIES_CAN_SHADE
                                  | XFW_WINDOW_CAPABILITIES_CAN_RESIZE
                                  | XFW_WINDOW_CAPABILITIES_CAN_MOVE);
        }

        capabilities_changed_mask = window->priv->capabilities ^ new_capabilities;
        window->priv->capabilities = new_capabilities;
    } else {
        state_changed_mask = 0;
        capabilities_changed_mask = 0;
    }

    if (pending->monitors_to_add != NULL) {
        GList *last = g_list_last(window->priv->monitors);
        if (last == NULL) {
            window->priv->monitors = pending->monitors_to_add;
        } else {
            last->next = pending->monitors_to_add;
            pending->monitors_to_add->prev = last;
        }
    }

    if (pending->monitors_to_remove != NULL) {
        for (GList *l = pending->monitors_to_remove; l != NULL; l = l->next) {
            window->priv->monitors = g_list_remove(window->priv->monitors, l->data);
        }
    }

    if (pending->icon_changed) {
        g_clear_pointer(&window->priv->icon_name, g_free);
        window->priv->icon_name = g_steal_pointer(&pending->new_icon_name);
        g_clear_list(&window->priv->icon_sizes, g_free);
        window->priv->icon_sizes = g_steal_pointer(&pending->new_icon_sizes);

        g_clear_object(&window->priv->icon);
        if (window->priv->icon_name) {
            window->priv->icon = g_themed_icon_new_with_default_fallbacks(window->priv->icon_name);
        } else if (window->priv->icon_sizes != NULL) {
            window->priv->icon = G_ICON(_xfw_wl_raster_icon_new(window));
        }
    }

    if (pending->workspace_changed) {
        window->priv->workspace = pending->new_workspace;
    }

    if (pending->new_app_id != NULL) {
        pending->new_app_id = NULL;
        g_object_notify(G_OBJECT(window), "application");
        g_signal_emit_by_name(window, "icon-changed");
        g_object_notify(G_OBJECT(window), "class-ids");
        g_signal_emit_by_name(window, "class-changed");
    }

    if (pending->new_name != NULL) {
        pending->new_name = NULL;
        g_object_notify(G_OBJECT(window), "name");
        g_signal_emit_by_name(window, "name-changed");
    }

    if (state_changed_mask != 0) {
        g_object_notify(G_OBJECT(window), "state");
        g_signal_emit_by_name(window, "state-changed", state_changed_mask, window->priv->state);

        if (window->priv->created_emitted && (old_state & XFW_WINDOW_STATE_ACTIVE) != (window->priv->state & XFW_WINDOW_STATE_ACTIVE)) {
            XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));

            if (window->priv->state & XFW_WINDOW_STATE_ACTIVE) {
                _xfw_screen_set_active_window(screen, XFW_WINDOW(window));
            } else if (xfw_screen_get_active_window(screen) == XFW_WINDOW(window)) {
                _xfw_screen_set_active_window(screen, NULL);
            }
        }
    }

    if (capabilities_changed_mask != 0) {
        g_object_notify(G_OBJECT(window), "capabilities");
        g_signal_emit_by_name(window, "capabilities-changed", capabilities_changed_mask, window->priv->capabilities);
    }

    if (pending->monitors_to_add != NULL || pending->monitors_to_remove != NULL) {
        pending->monitors_to_add = NULL;
        g_clear_pointer(&pending->monitors_to_remove, g_list_free);
        g_object_notify(G_OBJECT(window), "monitors");
    }

    if (pending->icon_changed) {
        pending->icon_changed = FALSE;
        g_object_notify(G_OBJECT(window), "gicon");
        g_signal_emit_by_name(window, "icon-changed");
    }

    if (pending->workspace_changed) {
        pending->workspace_changed = FALSE;
        pending->new_workspace = NULL;
        g_object_notify(G_OBJECT(window), "workspace");
        g_signal_emit_by_name(window, "workspace-changed");
    }
}

static void
wlr_toplevel_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, const char *app_id) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    if (app_id == NULL || *app_id == '\0' || g_strcmp0(app_id, window->priv->app_id) == 0) {
        return;
    }

    g_free(window->priv->pending.new_app_id);
    window->priv->pending.new_app_id = g_strdup(app_id);
}

static void
wlr_toplevel_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, const char *title) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    g_free(window->priv->pending.new_name);
    window->priv->pending.new_name = title != NULL ? g_strdup(title) : g_strdup("");
}

static void
wlr_toplevel_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct wl_array *wl_state) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);
    XfwWindowState new_state = XFW_WINDOW_STATE_NONE;
    enum zwlr_foreign_toplevel_handle_v1_state *item;

    wl_array_for_each(item, wl_state) {
        for (size_t i = 0; i < G_N_ELEMENTS(wlr_state_converters); ++i) {
            if (wlr_state_converters[i].proto_state == *item) {
                new_state |= wlr_state_converters[i].state_bit;
                break;
            }
        }
    }

    window->priv->pending.new_wlr_state = new_state;
    window->priv->pending.wlr_state_changed = TRUE;
}

static void
wlr_toplevel_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct zwlr_foreign_toplevel_handle_v1 *wl_parent) {
}

static gboolean
free_pending_outputs(gpointer data) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);
    g_clear_list(&window->priv->pending_outputs, NULL);
    window->priv->pending_outputs_id = 0;
    return FALSE;
}

static gint
find_monitor_for_output(gconstpointer data, gconstpointer user_data) {
    XfwMonitorWayland *monitor = XFW_MONITOR_WAYLAND((gpointer)data);
    const struct wl_output *output = user_data;
    return output == _xfw_monitor_wayland_get_wl_output(monitor) ? 0 : -1;
}

static void
wlr_toplevel_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct wl_output *output) {
    g_debug("toplevel %u output_enter", wl_proxy_get_id((struct wl_proxy *)wl_toplevel));

    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    GList *to_remove = g_list_find_custom(window->priv->pending.monitors_to_remove, output, find_monitor_for_output);
    if (to_remove != NULL) {
        window->priv->pending.monitors_to_remove = g_list_delete_link(window->priv->pending.monitors_to_remove, to_remove);
    } else {
        XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
        GList *monitors = xfw_screen_get_monitors(screen);

        GList *l = monitors;
        for (; l != NULL; l = l->next) {
            XfwMonitorWayland *monitor = XFW_MONITOR_WAYLAND(l->data);
            if (output == _xfw_monitor_wayland_get_wl_output(monitor)) {
                if (g_list_find(window->priv->monitors, monitor) == NULL
                    && g_list_find(window->priv->pending.monitors_to_add, monitor) == NULL) {
                    window->priv->pending.monitors_to_add = g_list_prepend(window->priv->pending.monitors_to_add, monitor);
                }
                break;
            }
        }

        // Sometimes the output_enter event is emitted before the XfwScreen monitor list has
        // been updated, so you have to wait for the XfwScreen::monitor-added signal to update
        // the window's monitor list. However, output_enter is emitted for two wl_outputs, only
        // one of which is of interest to us, and there is no guarantee that output_leave will
        // always be emitted when the output is destroyed. Pending wl_outputs can therefore
        // accumulate, without any criteria or good time to clean up the list (we may still
        // need them when XfwScreen::monitor-{added,removed} are emitted). The only solution
        // therefore seems to be to clean up the list a few seconds after a series of changes.
        if (l == NULL) {
            window->priv->pending_outputs = g_list_prepend(window->priv->pending_outputs, output);
            if (window->priv->pending_outputs_id != 0) {
                g_source_remove(window->priv->pending_outputs_id);
            }
            window->priv->pending_outputs_id = g_timeout_add_seconds(10, free_pending_outputs, window);
        }
    }
}

static void
wlr_toplevel_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel, struct wl_output *output) {
    g_debug("toplevel %u output_leave", wl_proxy_get_id((struct wl_proxy *)wl_toplevel));

    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    GList *to_add = g_list_find_custom(window->priv->pending.monitors_to_add, output, find_monitor_for_output);
    if (to_add != NULL) {
        window->priv->pending.monitors_to_add = g_list_delete_link(window->priv->pending.monitors_to_add, to_add);
    } else {
        XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
        GList *monitors = xfw_screen_get_monitors(screen);

        for (GList *l = monitors; l != NULL; l = l->next) {
            XfwMonitorWayland *monitor = XFW_MONITOR_WAYLAND(l->data);
            if (output == _xfw_monitor_wayland_get_wl_output(monitor)) {
                GList *lm = g_list_find(window->priv->monitors, monitor);
                if (lm != NULL && g_list_find(window->priv->pending.monitors_to_remove, monitor) == NULL) {
                    window->priv->pending.monitors_to_remove = g_list_prepend(window->priv->pending.monitors_to_remove, lm->data);
                }
                break;
            }
        }
    }
}

static void
wlr_toplevel_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);
    g_signal_emit_by_name(window, "closed");
}

static void
wlr_toplevel_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *wl_toplevel) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    if (!window->priv->created_emitted) {
        gint dones_needed = window->priv->xfce_handle != NULL ? 2 : 1;
        if (++window->priv->initial_dones_seen >= dones_needed) {
            XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));

            xfw_window_commit_changes(window);

            // nothing in the protocol ensures that the app id is set and we need an app
            if (window->priv->app == NULL) {
                wlr_toplevel_app_id(window, window->priv->wlr_handle, "UnknownAppID");
                xfw_window_commit_changes(window);
            }

            window->priv->created_emitted = TRUE;
            g_signal_emit_by_name(screen, "window-opened", window);
            if (window->priv->state & XFW_WINDOW_STATE_ACTIVE) {
                _xfw_screen_set_active_window(screen, XFW_WINDOW(window));
            }
        }
    } else {
        xfw_window_commit_changes(window);
    }
}

static void
xfce_toplevel_state(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, struct wl_array *xfce_state) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);
    XfwWindowState new_state = XFW_WINDOW_STATE_NONE;
    enum xfce_foreign_toplevel_handle_v1_state *item;

    wl_array_for_each(item, xfce_state) {
        for (size_t i = 0; i < G_N_ELEMENTS(xfce_state_converters); ++i) {
            if (xfce_state_converters[i].proto_state == *item) {
                new_state |= xfce_state_converters[i].state_bit;
                break;
            }
        }
    }

    window->priv->pending.new_xfce_state = new_state;
    window->priv->pending.xfce_state_changed = TRUE;
}

static void
xfce_toplevel_icon_name(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, const char *name) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    g_free(window->priv->pending.new_icon_name);
    window->priv->pending.new_icon_name = g_strdup(name);
    window->priv->pending.icon_changed = TRUE;
}

static void
xfce_toplevel_icon_size(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, uint32_t size, uint32_t scale) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    IconSize *icon_size = icon_size_new(size, scale);
    window->priv->pending.new_icon_sizes = g_list_prepend(window->priv->pending.new_icon_sizes, icon_size);
    window->priv->pending.icon_changed = TRUE;
}

static void
xfce_toplevel_no_icon(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);

    // If these are not NULL then the compositor is broken, but...
    g_clear_pointer(&window->priv->pending.new_icon_name, g_free);
    g_clear_list(&window->priv->pending.new_icon_sizes, g_free);
    window->priv->pending.icon_changed = TRUE;
}

static void
xfce_toplevel_workspace_enter(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, struct ext_workspace_handle_v1 *ext_workspace) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);
    XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
    XfwWorkspaceManager *manager = xfw_screen_get_workspace_manager(screen);
    XfwWorkspace *workspace = _xfw_workspace_manager_wayland_workspace_for_handle(XFW_WORKSPACE_MANAGER_WAYLAND(manager), ext_workspace);

    XfwWorkspace *current = window->priv->pending.workspace_changed ? window->priv->pending.new_workspace : window->priv->workspace;
    if (workspace != NULL && current != workspace) {
        window->priv->pending.new_workspace = workspace;
        window->priv->pending.workspace_changed = TRUE;
    }
}

static void
xfce_toplevel_workspace_leave(void *data, struct xfce_foreign_toplevel_handle_v1 *xfce_toplevel, struct ext_workspace_handle_v1 *ext_workspace) {
    XfwWindowWayland *window = XFW_WINDOW_WAYLAND(data);
    XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
    XfwWorkspaceManager *manager = xfw_screen_get_workspace_manager(screen);
    XfwWorkspace *workspace = _xfw_workspace_manager_wayland_workspace_for_handle(XFW_WORKSPACE_MANAGER_WAYLAND(manager), ext_workspace);

    XfwWorkspace *current = window->priv->pending.workspace_changed ? window->priv->pending.new_workspace : window->priv->workspace;
    if (workspace != NULL && current == workspace) {
        window->priv->pending.new_workspace = NULL;
        window->priv->pending.workspace_changed = TRUE;
    }
}

static void
monitor_added(XfwScreen *screen, XfwMonitor *monitor, XfwWindowWayland *window) {
    for (GList *l = window->priv->pending_outputs; l != NULL; l = l->next) {
        if (l->data == _xfw_monitor_wayland_get_wl_output(XFW_MONITOR_WAYLAND(monitor))) {
            window->priv->monitors = g_list_prepend(window->priv->monitors, monitor);
            g_object_notify(G_OBJECT(window), "monitors");
            break;
        }
    }
}

static void
monitor_removed(XfwScreen *screen, XfwMonitor *monitor, XfwWindowWayland *window) {
    GList *lm = g_list_find(window->priv->monitors, monitor);
    if (lm != NULL) {
        window->priv->monitors = g_list_delete_link(window->priv->monitors, lm);
        g_object_notify(G_OBJECT(window), "monitors");
    }
}

static IconSize *
icon_size_new(uint32_t size, uint32_t scale) {
    IconSize *icon_size = g_new0(IconSize, 1);
    icon_size->size = size;
    icon_size->scale = scale;
    return icon_size;
}


struct zwlr_foreign_toplevel_handle_v1 *
_xfw_window_wayland_get_wlr_handle(XfwWindowWayland *window) {
    return window->priv->wlr_handle;
}

struct xfce_foreign_toplevel_handle_v1 *
_xfw_window_wayland_get_xfce_handle(XfwWindowWayland *window) {
    return window->priv->xfce_handle;
}

GList *
_xfw_window_wayland_get_icon_sizes(XfwWindowWayland *window) {
    return window->priv->icon_sizes;
}
