/*
 * Copyright (c) 2022 Gaël Bonithon <gael@xfce.org>
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

#include <gtk/gtk.h>

#include "libxfce4windowing-private.h"
#include "xfw-application-private.h"
#include "xfw-application-wayland.h"
#include "xfw-screen-wayland.h"

enum {
    PROP_0,
    PROP_APP_ID,
};

struct _XfwApplicationWaylandPrivate {
    gchar *app_id;
    gchar *name;
    gchar *icon_name;
    GList *windows;
    GList *instances;
};

static GHashTable *app_ids = NULL;

static void xfw_application_wayland_constructed(GObject *obj);
static void xfw_application_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_application_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_application_wayland_finalize(GObject *obj);
static const gchar *xfw_application_wayland_get_class_id(XfwApplication *app);
static const gchar *xfw_application_wayland_get_name(XfwApplication *app);
static GIcon *xfw_application_wayland_get_gicon(XfwApplication *app);
static GList *xfw_application_wayland_get_windows(XfwApplication *app);
static GList *xfw_application_wayland_get_instances(XfwApplication *app);
static XfwApplicationInstance *xfw_application_wayland_get_instance(XfwApplication *app, XfwWindow *window);

static void toggle_notify(gpointer app, GObject *window, gboolean is_last_ref);


G_DEFINE_FINAL_TYPE_WITH_PRIVATE(XfwApplicationWayland, xfw_application_wayland, XFW_TYPE_APPLICATION)


static void
xfw_application_wayland_class_init(XfwApplicationWaylandClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    XfwApplicationClass *app_class = XFW_APPLICATION_CLASS(klass);

    gklass->constructed = xfw_application_wayland_constructed;
    gklass->set_property = xfw_application_wayland_set_property;
    gklass->get_property = xfw_application_wayland_get_property;
    gklass->finalize = xfw_application_wayland_finalize;

    app_class->get_class_id = xfw_application_wayland_get_class_id;
    app_class->get_name = xfw_application_wayland_get_name;
    app_class->get_gicon = xfw_application_wayland_get_gicon;
    app_class->get_windows = xfw_application_wayland_get_windows;
    app_class->get_instances = xfw_application_wayland_get_instances;
    app_class->get_instance = xfw_application_wayland_get_instance;

    g_object_class_install_property(gklass,
                                    PROP_APP_ID,
                                    g_param_spec_string("app-id",
                                                        "app-id",
                                                        "app-id",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_application_wayland_init(XfwApplicationWayland *app) {
    app->priv = xfw_application_wayland_get_instance_private(app);
}

static void
xfw_application_wayland_constructed(GObject *obj) {
    XfwApplicationWaylandPrivate *priv = XFW_APPLICATION_WAYLAND(obj)->priv;
    GDesktopAppInfo *app_info;

    g_hash_table_insert(app_ids, priv->app_id, obj);

    app_info = _xfw_g_desktop_app_info_get(priv->app_id);
    if (app_info != NULL) {
        gchar *name = g_desktop_app_info_get_string(app_info, G_KEY_FILE_DESKTOP_KEY_NAME);
        gchar *icon_name = g_desktop_app_info_get_string(app_info, G_KEY_FILE_DESKTOP_KEY_ICON);
        if (name != NULL) {
            priv->name = name;
            g_object_notify(obj, "name");
        }
        if (icon_name != NULL) {
            priv->icon_name = icon_name;
            g_signal_emit_by_name(obj, "icon-changed");
        }
        g_object_unref(app_info);
    }
    if (priv->name == NULL) {
        priv->name = g_strdup_printf("%c%s", g_unichar_totitle(*priv->app_id), priv->app_id + 1);
        g_object_notify(obj, "name");
    }

    G_OBJECT_CLASS(xfw_application_wayland_parent_class)->constructed(obj);
}

static void
xfw_application_wayland_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwApplicationWaylandPrivate *priv = XFW_APPLICATION_WAYLAND(obj)->priv;

    switch (prop_id) {
        case PROP_APP_ID:
            priv->app_id = g_value_dup_string(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_application_wayland_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwApplicationWaylandPrivate *priv = XFW_APPLICATION_WAYLAND(obj)->priv;

    switch (prop_id) {
        case PROP_APP_ID:
            g_value_set_string(value, priv->app_id);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_application_wayland_finalize(GObject *obj) {
    XfwApplicationWaylandPrivate *priv = XFW_APPLICATION_WAYLAND(obj)->priv;

    g_hash_table_remove(app_ids, priv->app_id);
    if (g_hash_table_size(app_ids) == 0) {
        g_clear_pointer(&app_ids, g_hash_table_destroy);
    }

    g_free(priv->app_id);
    g_free(priv->name);
    g_free(priv->icon_name);
    g_list_free(priv->windows);
    g_list_free(priv->instances);

    G_OBJECT_CLASS(xfw_application_wayland_parent_class)->finalize(obj);
}

static const gchar *
xfw_application_wayland_get_class_id(XfwApplication *app) {
    return XFW_APPLICATION_WAYLAND(app)->priv->app_id;
}

static const gchar *
xfw_application_wayland_get_name(XfwApplication *app) {
    return XFW_APPLICATION_WAYLAND(app)->priv->name;
}

static GIcon *
xfw_application_wayland_get_gicon(XfwApplication *app) {
    GIcon *gicon = _xfw_application_wayland_get_gicon_no_fallback(XFW_APPLICATION_WAYLAND(app));

    if (gicon != NULL) {
        return gicon;
    } else {
        return g_themed_icon_new_with_default_fallbacks(XFW_APPLICATION_FALLBACK_ICON_NAME);
    }
}

static GList *
xfw_application_wayland_get_windows(XfwApplication *app) {
    return XFW_APPLICATION_WAYLAND(app)->priv->windows;
}

static GList *
xfw_application_wayland_get_instances(XfwApplication *app) {
    return XFW_APPLICATION_WAYLAND(app)->priv->instances;
}

static XfwApplicationInstance *
xfw_application_wayland_get_instance(XfwApplication *app, XfwWindow *window) {
    // TODO
    return NULL;
}

static void
window_closed(XfwWindowWayland *window, XfwApplicationWayland *app) {
    g_signal_handlers_disconnect_by_data(window, app);
    app->priv->windows = g_list_remove(app->priv->windows, window);
    g_object_notify(G_OBJECT(app), "windows");
}

static void
window_application_changed(XfwWindowWayland *window, GParamSpec *pspec, XfwApplicationWayland *app) {
    if (XFW_APPLICATION(app) != xfw_window_get_application(XFW_WINDOW(window))) {
        g_signal_handlers_disconnect_by_data(window, app);
        app->priv->windows = g_list_remove(app->priv->windows, window);
        g_object_notify(G_OBJECT(app), "windows");
    }
}

static void
weak_notify(gpointer window, GObject *app) {
    g_signal_handlers_disconnect_by_data(window, app);
    g_object_remove_toggle_ref(window, toggle_notify, app);
}

static void
toggle_notify(gpointer app, GObject *window, gboolean is_last_ref) {
    g_object_weak_unref(app, weak_notify, window);
    g_object_remove_toggle_ref(window, toggle_notify, app);
}

XfwApplicationWayland *
_xfw_application_wayland_get(XfwWindowWayland *window, const gchar *app_id) {
    XfwApplicationWayland *app = NULL;

    if (app_ids == NULL) {
        app_ids = g_hash_table_new(g_str_hash, g_str_equal);
    } else {
        app = g_hash_table_lookup(app_ids, app_id);
    }

    if (app == NULL) {
        app = g_object_new(XFW_TYPE_APPLICATION_WAYLAND,
                           "app-id", app_id,
                           NULL);
    } else {
        g_object_ref(app);
    }

    // avoid reference cycle
    g_object_add_toggle_ref(G_OBJECT(window), toggle_notify, app);
    g_object_weak_ref(G_OBJECT(app), weak_notify, window);

    app->priv->windows = g_list_prepend(app->priv->windows, window);
    g_signal_connect(window, "closed", G_CALLBACK(window_closed), app);
    g_signal_connect(window, "notify::application", G_CALLBACK(window_application_changed), app);
    g_object_notify(G_OBJECT(app), "windows");

    return app;
}

GIcon *
_xfw_application_wayland_get_gicon_no_fallback(XfwApplicationWayland *app) {
    XfwApplicationWaylandPrivate *priv = XFW_APPLICATION_WAYLAND(app)->priv;
    return _xfw_g_icon_new(priv->icon_name);
}
