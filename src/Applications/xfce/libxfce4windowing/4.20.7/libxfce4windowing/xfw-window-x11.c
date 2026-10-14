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

#include <libwnck/libwnck.h>

#include "libxfce4windowing-private.h"
#include "xfw-application-x11.h"
#include "xfw-screen-x11.h"
#include "xfw-screen.h"
#include "xfw-util.h"
#include "xfw-window-private.h"
#include "xfw-window-x11.h"
#include "xfw-wnck-icon.h"
#include "xfw-workspace-x11.h"
#include "xfw-x11.h"
#include "libxfce4windowing-visibility.h"

enum {
    PROP_0,
    PROP_WNCK_WINDOW,
};

struct _XfwWindowX11Private {
    WnckWindow *wnck_window;

    const gchar **class_ids;
    XfwWindowType window_type;
    XfwWindowState state;
    XfwWindowCapabilities capabilities;
    GdkRectangle geometry;
    XfwWorkspace *workspace;
    GList *monitors;
    XfwApplication *app;
};

static void xfw_window_x11_constructed(GObject *obj);
static void xfw_window_x11_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_window_x11_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_window_x11_finalize(GObject *obj);

static const gchar *const *xfw_window_x11_get_class_ids(XfwWindow *window);
static const gchar *xfw_window_x11_get_name(XfwWindow *window);
static GIcon *xfw_window_x11_get_gicon(XfwWindow *window);
static XfwWindowType xfw_window_x11_get_window_type(XfwWindow *window);
static XfwWindowState xfw_window_x11_get_state(XfwWindow *window);
static XfwWindowCapabilities xfw_window_x11_get_capabilities(XfwWindow *window);
static GdkRectangle *xfw_window_x11_get_geometry(XfwWindow *window);
static XfwWorkspace *xfw_window_x11_get_workspace(XfwWindow *window);
static GList *xfw_window_x11_get_monitors(XfwWindow *window);
static XfwApplication *xfw_window_x11_get_application(XfwWindow *window);
static gboolean xfw_window_x11_activate(XfwWindow *window, XfwSeat *seat, guint64 event_timestamp, GError **error);
static gboolean xfw_window_x11_close(XfwWindow *window, guint64 event_timestamp, GError **error);
static gboolean xfw_window_x11_start_move(XfwWindow *window, GError **error);
static gboolean xfw_window_x11_start_resize(XfwWindow *window, GError **error);
static gboolean xfw_window_x11_set_geometry(XfwWindow *window, const GdkRectangle *rect, GError **error);
static gboolean xfw_window_x11_set_button_geometry(XfwWindow *window, GdkWindow *relative_to, const GdkRectangle *rect, GError **error);
static gboolean xfw_window_x11_move_to_workspace(XfwWindow *window, XfwWorkspace *workspace, GError **error);
static gboolean xfw_window_x11_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error);
static gboolean xfw_window_x11_set_maximized(XfwWindow *window, gboolean is_maximized, GError **error);
static gboolean xfw_window_x11_set_fullscreen(XfwWindow *window, gboolean is_fullscreen, GError **error);
static gboolean xfw_window_x11_set_skip_pager(XfwWindow *window, gboolean is_skip_pager, GError **error);
static gboolean xfw_window_x11_set_skip_tasklist(XfwWindow *window, gboolean is_skip_tasklist, GError **error);
static gboolean xfw_window_x11_set_pinned(XfwWindow *window, gboolean is_pinned, GError **error);
static gboolean xfw_window_x11_set_shaded(XfwWindow *window, gboolean is_shaded, GError **error);
static gboolean xfw_window_x11_set_above(XfwWindow *window, gboolean is_above, GError **error);
static gboolean xfw_window_x11_set_below(XfwWindow *window, gboolean is_below, GError **error);
static gboolean xfw_window_x11_is_on_workspace(XfwWindow *window, XfwWorkspace *workspace);
static gboolean xfw_window_x11_is_in_viewport(XfwWindow *window, XfwWorkspace *workspace);

static void class_changed(WnckWindow *wnck_window, XfwWindowX11 *window);
static void name_changed(WnckWindow *wnck_window, XfwWindowX11 *window);
static void icon_changed(WnckWindow *wnck_window, XfwWindowX11 *window);
static void app_name_changed(XfwApplication *app, GParamSpec *pspec, XfwWindowX11 *window);
static void type_changed(WnckWindow *wnck_window, XfwWindowX11 *window);
static void state_changed(WnckWindow *wnck_window, WnckWindowState changed_mask, WnckWindowState new_state, XfwWindowX11 *window);
static void actions_changed(WnckWindow *wnck_window, WnckWindowActions wnck_changed_mask, WnckWindowActions wnck_new_actions, XfwWindowX11 *window);
static void geometry_changed(WnckWindow *wnck_window, XfwWindowX11 *window);
static void monitor_added(XfwScreen *screen, XfwMonitor *monitor, XfwWindowX11 *window);
static void monitor_removed(XfwScreen *screen, XfwMonitor *monitor, XfwWindowX11 *window);
static void workspace_changed(WnckWindow *wnck_window, XfwWindowX11 *window);

static XfwWindowType convert_type(WnckWindowType wnck_type);
static XfwWindowState convert_state(WnckWindow *wnck_window, WnckWindowState wnck_state);
static XfwWindowCapabilities convert_capabilities(WnckWindow *wnck_window, WnckWindowActions wnck_actions);


G_DEFINE_FINAL_TYPE_WITH_PRIVATE(XfwWindowX11, xfw_window_x11, XFW_TYPE_WINDOW)


static void
xfw_window_x11_class_init(XfwWindowX11Class *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    XfwWindowClass *window_class = XFW_WINDOW_CLASS(klass);

    gklass->constructed = xfw_window_x11_constructed;
    gklass->set_property = xfw_window_x11_set_property;
    gklass->get_property = xfw_window_x11_get_property;
    gklass->finalize = xfw_window_x11_finalize;

    window_class->get_class_ids = xfw_window_x11_get_class_ids;
    window_class->get_name = xfw_window_x11_get_name;
    window_class->get_gicon = xfw_window_x11_get_gicon;
    window_class->get_window_type = xfw_window_x11_get_window_type;
    window_class->get_state = xfw_window_x11_get_state;
    window_class->get_capabilities = xfw_window_x11_get_capabilities;
    window_class->get_geometry = xfw_window_x11_get_geometry;
    window_class->get_workspace = xfw_window_x11_get_workspace;
    window_class->get_monitors = xfw_window_x11_get_monitors;
    window_class->get_application = xfw_window_x11_get_application;
    window_class->activate = xfw_window_x11_activate;
    window_class->close = xfw_window_x11_close;
    window_class->start_move = xfw_window_x11_start_move;
    window_class->start_resize = xfw_window_x11_start_resize;
    window_class->set_geometry = xfw_window_x11_set_geometry;
    window_class->set_button_geometry = xfw_window_x11_set_button_geometry;
    window_class->move_to_workspace = xfw_window_x11_move_to_workspace;
    window_class->set_minimized = xfw_window_x11_set_minimized;
    window_class->set_maximized = xfw_window_x11_set_maximized;
    window_class->set_fullscreen = xfw_window_x11_set_fullscreen;
    window_class->set_skip_pager = xfw_window_x11_set_skip_pager;
    window_class->set_skip_tasklist = xfw_window_x11_set_skip_tasklist;
    window_class->set_pinned = xfw_window_x11_set_pinned;
    window_class->set_shaded = xfw_window_x11_set_shaded;
    window_class->set_above = xfw_window_x11_set_above;
    window_class->set_below = xfw_window_x11_set_below;
    window_class->is_on_workspace = xfw_window_x11_is_on_workspace;
    window_class->is_in_viewport = xfw_window_x11_is_in_viewport;

    g_object_class_install_property(gklass,
                                    PROP_WNCK_WINDOW,
                                    g_param_spec_object("wnck-window",
                                                        "wnck-window",
                                                        "wnck-window",
                                                        WNCK_TYPE_WINDOW,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_window_x11_init(XfwWindowX11 *window) {
    window->priv = xfw_window_x11_get_instance_private(window);
}

static void
xfw_window_x11_constructed(GObject *obj) {
    XfwWindowX11 *window = XFW_WINDOW_X11(obj);
    XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
    const gchar *class_name = wnck_window_get_class_group_name(window->priv->wnck_window);
    const gchar *instance_name = wnck_window_get_class_instance_name(window->priv->wnck_window);

    window->priv->class_ids = g_new0(const gchar *, 3);
    if (class_name != NULL && *class_name != '\0') {
        window->priv->class_ids[0] = class_name;
        window->priv->class_ids[1] = instance_name;
    } else {
        window->priv->class_ids[0] = instance_name;
    }
    window->priv->window_type = convert_type(wnck_window_get_window_type(window->priv->wnck_window));
    window->priv->state = convert_state(window->priv->wnck_window, wnck_window_get_state(window->priv->wnck_window));
    wnck_window_get_geometry(window->priv->wnck_window,
                             &window->priv->geometry.x, &window->priv->geometry.y,
                             &window->priv->geometry.width, &window->priv->geometry.height);
    for (GList *l = xfw_screen_get_monitors(screen); l != NULL; l = l->next) {
        XfwMonitor *monitor = XFW_MONITOR(l->data);
        GdkRectangle geom;
        xfw_monitor_get_physical_geometry(monitor, &geom);
        if (gdk_rectangle_intersect(&window->priv->geometry, &geom, NULL)) {
            window->priv->monitors = g_list_prepend(window->priv->monitors, monitor);
        }
    }
    window->priv->capabilities = convert_capabilities(window->priv->wnck_window, wnck_window_get_actions(window->priv->wnck_window));
    window->priv->workspace = _xfw_screen_x11_workspace_for_wnck_workspace(XFW_SCREEN_X11(screen),
                                                                           wnck_window_get_workspace(window->priv->wnck_window));
    if (window->priv->workspace != NULL) {
        g_object_ref(window->priv->workspace);
    }
    window->priv->app = XFW_APPLICATION(_xfw_application_x11_get(wnck_window_get_class_group(window->priv->wnck_window), window));

    g_signal_connect(window->priv->wnck_window, "class-changed", G_CALLBACK(class_changed), window);
    g_signal_connect(window->priv->wnck_window, "name-changed", G_CALLBACK(name_changed), window);
    g_signal_connect(window->priv->wnck_window, "icon-changed", G_CALLBACK(icon_changed), window);
    g_signal_connect(window->priv->app, "notify::name", G_CALLBACK(app_name_changed), window);
    g_signal_connect(window->priv->wnck_window, "type-changed", G_CALLBACK(type_changed), window);
    g_signal_connect(window->priv->wnck_window, "state-changed", G_CALLBACK(state_changed), window);
    g_signal_connect(window->priv->wnck_window, "actions-changed", G_CALLBACK(actions_changed), window);
    g_signal_connect(window->priv->wnck_window, "geometry-changed", G_CALLBACK(geometry_changed), window);
    g_signal_connect(window->priv->wnck_window, "workspace-changed", G_CALLBACK(workspace_changed), window);
    g_signal_connect(screen, "monitor-added", G_CALLBACK(monitor_added), window);
    g_signal_connect(screen, "monitor-removed", G_CALLBACK(monitor_removed), window);

    G_OBJECT_CLASS(xfw_window_x11_parent_class)->constructed(obj);
}

static void
xfw_window_x11_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWindowX11 *window = XFW_WINDOW_X11(obj);

    switch (prop_id) {
        case PROP_WNCK_WINDOW:
            window->priv->wnck_window = g_value_dup_object(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_window_x11_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWindow *window = XFW_WINDOW(obj);

    switch (prop_id) {
        case PROP_WNCK_WINDOW:
            g_value_set_object(value, XFW_WINDOW_X11(window)->priv->wnck_window);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_window_x11_finalize(GObject *obj) {
    XfwWindowX11 *window = XFW_WINDOW_X11(obj);

    g_signal_handlers_disconnect_by_data(window->priv->wnck_window, window);
    g_signal_handlers_disconnect_by_data(window->priv->app, window);
    g_signal_handlers_disconnect_by_data(_xfw_window_get_screen(XFW_WINDOW(window)), window);

    g_free(window->priv->class_ids);
    g_list_free(window->priv->monitors);
    g_object_unref(window->priv->app);
    if (window->priv->workspace != NULL) {
        g_object_unref(window->priv->workspace);
    }

    // to be released last
    g_object_unref(window->priv->wnck_window);

    G_OBJECT_CLASS(xfw_window_x11_parent_class)->finalize(obj);
}

static const gchar *const *
xfw_window_x11_get_class_ids(XfwWindow *window) {
    return XFW_WINDOW_X11(window)->priv->class_ids;
}

static const gchar *
xfw_window_x11_get_name(XfwWindow *window) {
    return wnck_window_get_name(XFW_WINDOW_X11(window)->priv->wnck_window);
}

static GIcon *
xfw_window_x11_get_gicon(XfwWindow *window) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;

    return _xfw_wnck_object_get_gicon(G_OBJECT(priv->wnck_window),
                                      NULL,
                                      priv->app != NULL ? _xfw_application_x11_get_icon_name(XFW_APPLICATION_X11(priv->app)) : NULL,
                                      XFW_WINDOW_FALLBACK_ICON_NAME);
}

static XfwWindowType
xfw_window_x11_get_window_type(XfwWindow *window) {
    return XFW_WINDOW_X11(window)->priv->window_type;
}

static XfwWindowState
xfw_window_x11_get_state(XfwWindow *window) {
    return XFW_WINDOW_X11(window)->priv->state;
}

static XfwWindowCapabilities
xfw_window_x11_get_capabilities(XfwWindow *window) {
    return XFW_WINDOW_X11(window)->priv->capabilities;
}

static GdkRectangle *
xfw_window_x11_get_geometry(XfwWindow *window) {
    return &XFW_WINDOW_X11(window)->priv->geometry;
}

static XfwWorkspace *
xfw_window_x11_get_workspace(XfwWindow *window) {
    return XFW_WINDOW_X11(window)->priv->workspace;
}

static GList *
xfw_window_x11_get_monitors(XfwWindow *window) {
    return XFW_WINDOW_X11(window)->priv->monitors;
}

static XfwApplication *
xfw_window_x11_get_application(XfwWindow *window) {
    return XFW_WINDOW_X11(window)->priv->app;
}

static gboolean
xfw_window_x11_activate(XfwWindow *window, XfwSeat *seat, guint64 event_timestamp, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;
    wnck_window_activate(priv->wnck_window, (guint32)event_timestamp);
    return TRUE;
}

static gboolean
xfw_window_x11_close(XfwWindow *window, guint64 event_timestamp, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;
    wnck_window_close(priv->wnck_window, (guint32)event_timestamp);
    return TRUE;
}

static gboolean
xfw_window_x11_start_move(XfwWindow *window, GError **error) {
    wnck_window_keyboard_move(XFW_WINDOW_X11(window)->priv->wnck_window);
    return TRUE;
}

static gboolean
xfw_window_x11_start_resize(XfwWindow *window, GError **error) {
    wnck_window_keyboard_size(XFW_WINDOW_X11(window)->priv->wnck_window);
    return TRUE;
}

static gboolean
xfw_window_x11_set_geometry(XfwWindow *window, const GdkRectangle *rect, GError **error) {
    WnckWindowMoveResizeMask mask = 0;
    if (rect->x >= 0) {
        mask |= WNCK_WINDOW_CHANGE_X;
    }
    if (rect->y >= 0) {
        mask |= WNCK_WINDOW_CHANGE_Y;
    }
    if (rect->width >= 0) {
        mask |= WNCK_WINDOW_CHANGE_WIDTH;
    }
    if (rect->height >= 0) {
        mask |= WNCK_WINDOW_CHANGE_HEIGHT;
    }
    wnck_window_set_geometry(XFW_WINDOW_X11(window)->priv->wnck_window, WNCK_WINDOW_GRAVITY_NORTHWEST, mask, rect->x, rect->y, rect->width, rect->height);
    return TRUE;
}

static gboolean
xfw_window_x11_set_button_geometry(XfwWindow *window, GdkWindow *relative_to, const GdkRectangle *rect, GError **error) {
    wnck_window_set_icon_geometry(XFW_WINDOW_X11(window)->priv->wnck_window, rect->x, rect->y, rect->width, rect->height);
    return TRUE;
}

static gboolean
xfw_window_x11_move_to_workspace(XfwWindow *window, XfwWorkspace *workspace, GError **error) {
    WnckWorkspace *wnck_workspace;

    g_return_val_if_fail(XFW_IS_WORKSPACE(workspace), FALSE);

    wnck_workspace = _xfw_workspace_x11_get_wnck_workspace(XFW_WORKSPACE_X11(workspace));
    wnck_window_move_to_workspace(XFW_WINDOW_X11(window)->priv->wnck_window, wnck_workspace);
    return TRUE;
}

static gboolean
xfw_window_x11_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;

    if (is_minimized) {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE) != 0) {
            wnck_window_minimize(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being minimized");
            }
            return FALSE;
        }
    } else {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE) != 0) {
            wnck_window_unminimize(priv->wnck_window, g_get_monotonic_time() / 1000);
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
xfw_window_x11_set_maximized(XfwWindow *window, gboolean is_maximized, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;

    if (is_maximized) {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE) != 0) {
            wnck_window_maximize(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being maximized");
            }
            return FALSE;
        }
    } else {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE) != 0) {
            wnck_window_unmaximize(priv->wnck_window);
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
xfw_window_x11_set_fullscreen(XfwWindow *window, gboolean is_fullscreen, GError **error) {
    XfwWindowX11 *xwindow = XFW_WINDOW_X11(window);

    if (is_fullscreen && (xwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN) == 0) {
        if (error != NULL) {
            *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being set fullscreen");
        }
        return FALSE;
    } else if (!is_fullscreen && (xwindow->priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN) == 0) {
        if (error != NULL) {
            *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being unset fullscreen");
        }
        return FALSE;
    } else {
        wnck_window_set_fullscreen(xwindow->priv->wnck_window, is_fullscreen);
        return TRUE;
    }
}

static gboolean
xfw_window_x11_set_skip_pager(XfwWindow *window, gboolean is_skip_pager, GError **error) {
    wnck_window_set_skip_pager(XFW_WINDOW_X11(window)->priv->wnck_window, is_skip_pager);
    return TRUE;
}

static gboolean
xfw_window_x11_set_skip_tasklist(XfwWindow *window, gboolean is_skip_tasklist, GError **error) {
    wnck_window_set_skip_tasklist(XFW_WINDOW_X11(window)->priv->wnck_window, is_skip_tasklist);
    return TRUE;
}

static gboolean
xfw_window_x11_set_pinned(XfwWindow *window, gboolean is_pinned, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;

    if (is_pinned) {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0) {
            wnck_window_pin(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being pinned");
            }
            return FALSE;
        }
    } else {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0) {
            wnck_window_unpin(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being unpinned");
            }
            return FALSE;
        }
    }
}

static gboolean
xfw_window_x11_set_shaded(XfwWindow *window, gboolean is_shaded, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;

    if (is_shaded) {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_SHADE) != 0) {
            wnck_window_shade(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being shaded");
            }
            return FALSE;
        }
    } else {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNSHADE) != 0) {
            wnck_window_unshade(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being unshaded");
            }
            return FALSE;
        }
    }
}

static gboolean
xfw_window_x11_set_above(XfwWindow *window, gboolean is_above, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;

    if (is_above) {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE) != 0) {
            wnck_window_make_above(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed above others");
            }
            return FALSE;
        }
    } else {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE) != 0) {
            wnck_window_unmake_above(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed back in the normal stacking order");
            }
            return FALSE;
        }
    }
}

static gboolean
xfw_window_x11_set_below(XfwWindow *window, gboolean is_below, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;

    if (is_below) {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW) != 0) {
            wnck_window_make_below(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed below others");
            }
            return FALSE;
        }
    } else {
        if ((priv->capabilities & XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW) != 0) {
            wnck_window_unmake_below(priv->wnck_window);
            return TRUE;
        } else {
            if (error != NULL) {
                *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_UNSUPPORTED, "This window does not currently support being placed back in the normal stacking order");
            }
            return FALSE;
        }
    }
}

static gboolean
xfw_window_x11_is_on_workspace(XfwWindow *window, XfwWorkspace *workspace) {
    return wnck_window_is_on_workspace(XFW_WINDOW_X11(window)->priv->wnck_window,
                                       _xfw_workspace_x11_get_wnck_workspace(XFW_WORKSPACE_X11(workspace)));
}

static gboolean
xfw_window_x11_is_in_viewport(XfwWindow *window, XfwWorkspace *workspace) {
    return wnck_window_is_in_viewport(XFW_WINDOW_X11(window)->priv->wnck_window,
                                      _xfw_workspace_x11_get_wnck_workspace(XFW_WORKSPACE_X11(workspace)));
}

static void
class_changed(WnckWindow *wnck_window, XfwWindowX11 *window) {
    const gchar *class_name = wnck_window_get_class_group_name(wnck_window);
    const gchar *instance_name = wnck_window_get_class_instance_name(wnck_window);
    if (class_name != NULL && *class_name != '\0') {
        window->priv->class_ids[0] = class_name;
        window->priv->class_ids[1] = instance_name;
    } else {
        window->priv->class_ids[0] = instance_name;
        window->priv->class_ids[1] = NULL;
    }
    g_object_notify(G_OBJECT(window), "class-ids");
    g_signal_emit_by_name(window, "class-changed");
}

static void
name_changed(WnckWindow *wnck_window, XfwWindowX11 *window) {
    g_object_notify(G_OBJECT(window), "name");
    g_signal_emit_by_name(window, "name-changed");
}

static void
icon_changed(WnckWindow *wnck_window, XfwWindowX11 *window) {
    _xfw_window_invalidate_icon(XFW_WINDOW(window));
    g_signal_emit_by_name(window, "icon-changed");
}

static void
app_name_changed(XfwApplication *app, GParamSpec *pspec, XfwWindowX11 *window) {
    _xfw_window_invalidate_icon(XFW_WINDOW(window));
    g_signal_emit_by_name(window, "icon-changed");
}

static void
type_changed(WnckWindow *wnck_window, XfwWindowX11 *window) {
    XfwWindowType old_type = window->priv->window_type;
    window->priv->window_type = convert_type(wnck_window_get_window_type(window->priv->wnck_window));
    g_object_notify(G_OBJECT(window), "type");
    g_signal_emit_by_name(window, "type-changed", old_type);
}

static void
state_changed(WnckWindow *wnck_window, WnckWindowState wnck_changed_mask, WnckWindowState wnck_new_state, XfwWindowX11 *window) {
    XfwWindowState old_state = window->priv->state;
    XfwWindowState new_state = convert_state(wnck_window, wnck_new_state);
    XfwWindowState changed_mask = old_state ^ new_state;

    if (changed_mask != XFW_WINDOW_STATE_NONE) {
        window->priv->state = new_state;
        g_object_notify(G_OBJECT(window), "state");
        g_signal_emit_by_name(window, "state-changed", changed_mask, new_state);
    }

    // Not all capability changes are reported by WnckWindow::actions-changed (e.g. shade/unshade) so we need to add this update
    actions_changed(wnck_window, 0, wnck_window_get_actions(wnck_window), window);
}

static void
actions_changed(WnckWindow *wnck_window, WnckWindowActions wnck_changed_mask, WnckWindowActions wnck_new_actions, XfwWindowX11 *window) {
    XfwWindowCapabilities old_capabilities = window->priv->capabilities;
    XfwWindowCapabilities new_capabilities = convert_capabilities(wnck_window, wnck_new_actions);
    XfwWindowCapabilities changed_mask = old_capabilities ^ new_capabilities;

    if (changed_mask != XFW_WINDOW_CAPABILITIES_NONE) {
        window->priv->capabilities = new_capabilities;
        g_object_notify(G_OBJECT(window), "capabilities");
        g_signal_emit_by_name(window, "capabilities-changed", changed_mask, new_capabilities);
    }
}

static void
geometry_changed(WnckWindow *wnck_window, XfwWindowX11 *window) {
    gboolean notify = FALSE;

    wnck_window_get_geometry(wnck_window,
                             &window->priv->geometry.x, &window->priv->geometry.y,
                             &window->priv->geometry.width, &window->priv->geometry.height);
    g_signal_emit_by_name(window, "geometry-changed");

    for (GList *lp = window->priv->monitors; lp != NULL;) {
        GList *lnext = lp->next;
        GdkRectangle geom;
        xfw_monitor_get_physical_geometry(XFW_MONITOR(lp->data), &geom);
        if (!gdk_rectangle_intersect(&window->priv->geometry, &geom, NULL)) {
            window->priv->monitors = g_list_delete_link(window->priv->monitors, lp);
            notify = TRUE;
        }
        lp = lnext;
    }

    for (GList *l = xfw_screen_get_monitors(_xfw_window_get_screen(XFW_WINDOW(window))); l != NULL; l = l->next) {
        XfwMonitor *monitor = XFW_MONITOR(l->data);
        GdkRectangle geom;
        xfw_monitor_get_physical_geometry(monitor, &geom);
        if (gdk_rectangle_intersect(&window->priv->geometry, &geom, NULL) && !g_list_find(window->priv->monitors, monitor)) {
            window->priv->monitors = g_list_prepend(window->priv->monitors, monitor);
            notify = TRUE;
        }
    }

    if (notify) {
        g_object_notify(G_OBJECT(window), "monitors");
    }
}

static void
monitor_added(XfwScreen *screen, XfwMonitor *monitor, XfwWindowX11 *window) {
    geometry_changed(window->priv->wnck_window, window);
}

static void
monitor_removed(XfwScreen *screen, XfwMonitor *monitor, XfwWindowX11 *window) {
    g_object_freeze_notify(G_OBJECT(window));

    GList *lp = g_list_find(window->priv->monitors, monitor);
    if (lp != NULL) {
        window->priv->monitors = g_list_delete_link(window->priv->monitors, lp);
        g_object_notify(G_OBJECT(window), "monitors");
    }
    geometry_changed(window->priv->wnck_window, window);

    g_object_thaw_notify(G_OBJECT(window));
}

static void
workspace_changed(WnckWindow *wnck_window, XfwWindowX11 *window) {
    XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
    XfwWorkspace *new_workspace = _xfw_screen_x11_workspace_for_wnck_workspace(XFW_SCREEN_X11(screen),
                                                                               wnck_window_get_workspace(wnck_window));
    XfwWorkspace *old_workspace = window->priv->workspace;

    if (old_workspace != new_workspace) {
        window->priv->workspace = new_workspace;
        if (window->priv->workspace != NULL) {
            g_object_ref(window->priv->workspace);
        }
    }

    // workspace-changed is also fired when the windows pinned state changes
    state_changed(wnck_window, 0, wnck_window_get_state(wnck_window), window);

    if (old_workspace != new_workspace) {
        g_object_notify(G_OBJECT(window), "workspace");
        g_signal_emit_by_name(window, "workspace-changed");
        if (old_workspace != NULL) {
            g_object_unref(old_workspace);
        }
    }
}

static XfwWindowType
convert_type(WnckWindowType wnck_type) {
    switch (wnck_type) {
        case WNCK_WINDOW_NORMAL:
            return XFW_WINDOW_TYPE_NORMAL;
        case WNCK_WINDOW_DESKTOP:
            return XFW_WINDOW_TYPE_DESKTOP;
        case WNCK_WINDOW_DOCK:
            return XFW_WINDOW_TYPE_DOCK;
        case WNCK_WINDOW_DIALOG:
            return XFW_WINDOW_TYPE_DIALOG;
        case WNCK_WINDOW_TOOLBAR:
            return XFW_WINDOW_TYPE_TOOLBAR;
        case WNCK_WINDOW_MENU:
            return XFW_WINDOW_TYPE_MENU;
        case WNCK_WINDOW_UTILITY:
            return XFW_WINDOW_TYPE_UTILITY;
        case WNCK_WINDOW_SPLASHSCREEN:
            return XFW_WINDOW_TYPE_SPLASHSCREEN;
    }
    return XFW_WINDOW_TYPE_NORMAL;
}

static const struct {
    WnckWindowState wnck_state_bits;
    XfwWindowState state_bit;
} state_converters[] = {
    { WNCK_WINDOW_STATE_MINIMIZED, XFW_WINDOW_STATE_MINIMIZED },
    { WNCK_WINDOW_STATE_MAXIMIZED_HORIZONTALLY | WNCK_WINDOW_STATE_MAXIMIZED_VERTICALLY, XFW_WINDOW_STATE_MAXIMIZED },
    { WNCK_WINDOW_STATE_FULLSCREEN, XFW_WINDOW_STATE_FULLSCREEN },
    { WNCK_WINDOW_STATE_SKIP_PAGER, XFW_WINDOW_STATE_SKIP_PAGER },
    { WNCK_WINDOW_STATE_SKIP_TASKLIST, XFW_WINDOW_STATE_SKIP_TASKLIST },
    { WNCK_WINDOW_STATE_ABOVE, XFW_WINDOW_STATE_ABOVE },
    { WNCK_WINDOW_STATE_BELOW, XFW_WINDOW_STATE_BELOW },
    { WNCK_WINDOW_STATE_DEMANDS_ATTENTION | WNCK_WINDOW_STATE_URGENT, XFW_WINDOW_STATE_URGENT },
};

static XfwWindowState
convert_state(WnckWindow *wnck_window, WnckWindowState wnck_state) {
    XfwWindowState state = XFW_WINDOW_STATE_NONE;
    for (size_t i = 0; i < G_N_ELEMENTS(state_converters); ++i) {
        if ((wnck_state & state_converters[i].wnck_state_bits) != 0) {
            state |= state_converters[i].state_bit;
        }
    }
    if (wnck_window_is_active(wnck_window)) {
        state |= XFW_WINDOW_STATE_ACTIVE;
    }
    if (wnck_window_is_pinned(wnck_window)) {
        state |= XFW_WINDOW_STATE_PINNED;
    }
    if (wnck_window_is_shaded(wnck_window)) {
        state |= XFW_WINDOW_STATE_SHADED;
    }
    return state;
}

static const struct {
    // Action bits we need to find in actions in order to enable capabilities_bits
    WnckWindowActions wnck_actions_bits;
    // State bits from the window state that we...
    WnckWindowState wnck_state_bits;
    // ... need or do not need to find in order to enable capabilities_bit
    gboolean need_wnck_state_bits_present;
    XfwWindowCapabilities capabilities_bit;
} capabilities_converters[] = {
    { WNCK_WINDOW_ACTION_MINIMIZE, WNCK_WINDOW_STATE_MINIMIZED, FALSE, XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE },
    { WNCK_WINDOW_ACTION_MAXIMIZE, WNCK_WINDOW_STATE_MAXIMIZED_HORIZONTALLY | WNCK_WINDOW_STATE_MAXIMIZED_VERTICALLY, FALSE, XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE },
    { WNCK_WINDOW_ACTION_FULLSCREEN, WNCK_WINDOW_STATE_FULLSCREEN, FALSE, XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN },
    { WNCK_WINDOW_ACTION_SHADE, WNCK_WINDOW_STATE_SHADED, FALSE, XFW_WINDOW_CAPABILITIES_CAN_SHADE },
    { WNCK_WINDOW_ACTION_UNMINIMIZE, WNCK_WINDOW_STATE_MINIMIZED, TRUE, XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE },
    { WNCK_WINDOW_ACTION_UNMAXIMIZE, WNCK_WINDOW_STATE_MAXIMIZED_HORIZONTALLY | WNCK_WINDOW_STATE_MAXIMIZED_VERTICALLY, TRUE, XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE },
    { WNCK_WINDOW_ACTION_FULLSCREEN, WNCK_WINDOW_STATE_FULLSCREEN, TRUE, XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN },
    { WNCK_WINDOW_ACTION_UNSHADE, WNCK_WINDOW_STATE_SHADED, TRUE, XFW_WINDOW_CAPABILITIES_CAN_UNSHADE },
    { WNCK_WINDOW_ACTION_MOVE, 0, FALSE, XFW_WINDOW_CAPABILITIES_CAN_MOVE },
    { WNCK_WINDOW_ACTION_RESIZE, 0, FALSE, XFW_WINDOW_CAPABILITIES_CAN_RESIZE },
    { WNCK_WINDOW_ACTION_ABOVE, WNCK_WINDOW_STATE_ABOVE, FALSE, XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE },
    { WNCK_WINDOW_ACTION_ABOVE, WNCK_WINDOW_STATE_ABOVE, TRUE, XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE },
    { WNCK_WINDOW_ACTION_BELOW, WNCK_WINDOW_STATE_BELOW, FALSE, XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW },
    { WNCK_WINDOW_ACTION_BELOW, WNCK_WINDOW_STATE_BELOW, TRUE, XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW },
    { WNCK_WINDOW_ACTION_CHANGE_WORKSPACE, 0, FALSE, XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE },
};
static XfwWindowCapabilities
convert_capabilities(WnckWindow *wnck_window, WnckWindowActions wnck_actions) {
    WnckWindowState wnck_state = wnck_window_get_state(wnck_window);
    XfwWindowCapabilities capabilities = XFW_WINDOW_CAPABILITIES_NONE;
    for (size_t i = 0; i < G_N_ELEMENTS(capabilities_converters); ++i) {
        if ((wnck_actions & capabilities_converters[i].wnck_actions_bits) != 0) {
            if ((capabilities_converters[i].need_wnck_state_bits_present
                 && (wnck_state & capabilities_converters[i].wnck_state_bits) != 0)
                || (!capabilities_converters[i].need_wnck_state_bits_present
                    && (wnck_state & capabilities_converters[i].wnck_state_bits) == 0))
            {
                capabilities |= capabilities_converters[i].capabilities_bit;
            }
        }
    }
    return capabilities;
}

/**
 * xfw_window_x11_get_xid:
 * @window: A #XfwWindow
 *
 * On X11, returns the platform-specific #Window handle to the underlying
 * window.
 *
 * It is an error to call this function if the application is not currently
 * running on X11.
 *
 * Return value: An X11 #Window handle.
 *
 * Since: 4.19.3
 **/
Window
xfw_window_x11_get_xid(XfwWindow *window) {
    g_return_val_if_fail(XFW_IS_WINDOW_X11(window), (Window)0);
    return wnck_window_get_xid(XFW_WINDOW_X11(window)->priv->wnck_window);
}

WnckWindow *
_xfw_window_x11_get_wnck_window(XfwWindowX11 *window) {
    return window->priv->wnck_window;
}

#define __XFW_WINDOW_X11_C__
#include "libxfce4windowing-visibility.c"
