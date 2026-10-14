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

#include "libxfce4windowing-private.h"
#include "xfw-application-private.h"
#include "xfw-application-x11.h"
#include "xfw-util.h"
#include "xfw-window.h"
#include "xfw-wnck-icon.h"

enum {
    PROP_0,
    PROP_WNCK_GROUP,
};

struct _XfwApplicationX11Private {
    WnckClassGroup *wnck_group;
    gchar *icon_name;
    GList *windows;
    GHashTable *instances;
    GList *instance_list;
};

static GHashTable *wnck_groups = NULL;

static void xfw_application_x11_constructed(GObject *obj);
static void xfw_application_x11_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_application_x11_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_application_x11_finalize(GObject *obj);
static const gchar *xfw_application_x11_get_class_id(XfwApplication *app);
static const gchar *xfw_application_x11_get_name(XfwApplication *app);
static GIcon *xfw_application_x11_get_gicon(XfwApplication *app);
static GList *xfw_application_x11_get_windows(XfwApplication *app);
static GList *xfw_application_x11_get_instances(XfwApplication *app);
static XfwApplicationInstance *xfw_application_x11_get_instance(XfwApplication *app, XfwWindow *window);

static void icon_changed(WnckClassGroup *wnck_group, XfwApplicationX11 *app);
static void name_changed(WnckClassGroup *wnck_group, XfwApplicationX11 *app);
static void toggle_notify(gpointer app, GObject *window, gboolean is_last_ref);


G_DEFINE_FINAL_TYPE_WITH_PRIVATE(XfwApplicationX11, xfw_application_x11, XFW_TYPE_APPLICATION)


static void
xfw_application_x11_class_init(XfwApplicationX11Class *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    XfwApplicationClass *app_class = XFW_APPLICATION_CLASS(klass);

    gklass->constructed = xfw_application_x11_constructed;
    gklass->set_property = xfw_application_x11_set_property;
    gklass->get_property = xfw_application_x11_get_property;
    gklass->finalize = xfw_application_x11_finalize;

    app_class->get_class_id = xfw_application_x11_get_class_id;
    app_class->get_name = xfw_application_x11_get_name;
    app_class->get_gicon = xfw_application_x11_get_gicon;
    app_class->get_windows = xfw_application_x11_get_windows;
    app_class->get_instances = xfw_application_x11_get_instances;
    app_class->get_instance = xfw_application_x11_get_instance;

    g_object_class_install_property(gklass,
                                    PROP_WNCK_GROUP,
                                    g_param_spec_object("wnck-group",
                                                        "wnck-group",
                                                        "wnck-group",
                                                        WNCK_TYPE_CLASS_GROUP,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_application_x11_init(XfwApplicationX11 *app) {
    app->priv = xfw_application_x11_get_instance_private(app);
}

static void
xfw_application_x11_constructed(GObject *obj) {
    XfwApplicationX11Private *priv = XFW_APPLICATION_X11(obj)->priv;

    g_hash_table_insert(wnck_groups, priv->wnck_group, obj);
    priv->instances = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, _xfw_application_instance_free);

    g_signal_connect(priv->wnck_group, "icon-changed", G_CALLBACK(icon_changed), obj);
    name_changed(priv->wnck_group, XFW_APPLICATION_X11(obj));
    g_signal_connect(priv->wnck_group, "name-changed", G_CALLBACK(name_changed), obj);

    G_OBJECT_CLASS(xfw_application_x11_parent_class)->constructed(obj);
}

static void
xfw_application_x11_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwApplicationX11Private *priv = XFW_APPLICATION_X11(obj)->priv;

    switch (prop_id) {
        case PROP_WNCK_GROUP:
            priv->wnck_group = g_value_dup_object(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_application_x11_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwApplicationX11Private *priv = XFW_APPLICATION_X11(obj)->priv;

    switch (prop_id) {
        case PROP_WNCK_GROUP:
            g_value_set_object(value, priv->wnck_group);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_application_x11_finalize(GObject *obj) {
    XfwApplicationX11Private *priv = XFW_APPLICATION_X11(obj)->priv;

    g_hash_table_remove(wnck_groups, priv->wnck_group);
    if (g_hash_table_size(wnck_groups) == 0) {
        g_clear_pointer(&wnck_groups, g_hash_table_destroy);
    }

    g_signal_handlers_disconnect_by_func(priv->wnck_group, icon_changed, obj);
    g_signal_handlers_disconnect_by_func(priv->wnck_group, name_changed, obj);

    g_free(priv->icon_name);
    g_list_free(priv->windows);
    g_hash_table_destroy(priv->instances);
    g_list_free(priv->instance_list);

    // to be released last
    g_object_unref(priv->wnck_group);

    G_OBJECT_CLASS(xfw_application_x11_parent_class)->finalize(obj);
}

static const gchar *
xfw_application_x11_get_class_id(XfwApplication *app) {
    return wnck_class_group_get_id(XFW_APPLICATION_X11(app)->priv->wnck_group);
}

static const gchar *
xfw_application_x11_get_name(XfwApplication *app) {
    return wnck_class_group_get_name(XFW_APPLICATION_X11(app)->priv->wnck_group);
}

static GIcon *
xfw_application_x11_get_gicon(XfwApplication *app) {
    XfwApplicationX11Private *priv = XFW_APPLICATION_X11(app)->priv;

    return _xfw_wnck_object_get_gicon(G_OBJECT(priv->wnck_group),
                                      priv->icon_name,
                                      NULL,
                                      XFW_APPLICATION_FALLBACK_ICON_NAME);
}

static GList *
xfw_application_x11_get_windows(XfwApplication *app) {
    return XFW_APPLICATION_X11(app)->priv->windows;
}

static GList *
xfw_application_x11_get_instances(XfwApplication *app) {
    return XFW_APPLICATION_X11(app)->priv->instance_list;
}

static XfwApplicationInstance *
xfw_application_x11_get_instance(XfwApplication *app, XfwWindow *window) {
    return g_hash_table_lookup(XFW_APPLICATION_X11(app)->priv->instances,
                               wnck_window_get_application(_xfw_window_x11_get_wnck_window(XFW_WINDOW_X11(window))));
}

static void
icon_changed(WnckClassGroup *wnck_group, XfwApplicationX11 *app) {
    _xfw_application_invalidate_icon(XFW_APPLICATION(app));
    g_signal_emit_by_name(app, "icon-changed");
}

static void
name_changed(WnckClassGroup *wnck_group, XfwApplicationX11 *app) {
    GDesktopAppInfo *app_info = _xfw_g_desktop_app_info_get(wnck_class_group_get_id(wnck_group));
    gchar *icon_name = NULL;

    if (app_info != NULL) {
        icon_name = g_desktop_app_info_get_string(app_info, G_KEY_FILE_DESKTOP_KEY_ICON);
        g_object_unref(app_info);
    }
    if (g_strcmp0(icon_name, app->priv->icon_name) != 0) {
        g_free(app->priv->icon_name);
        app->priv->icon_name = icon_name;
        _xfw_application_invalidate_icon(XFW_APPLICATION(app));
        g_signal_emit_by_name(app, "icon-changed");
    } else {
        g_free(icon_name);
    }
    g_object_notify(G_OBJECT(app), "name");
}

static gboolean
find_instance(gpointer key, gpointer value, gpointer user_data) {
    return g_list_find(((XfwApplicationInstance *)value)->windows, user_data) != NULL;
}

static void
window_closed(XfwWindowX11 *window, XfwApplicationX11 *app) {
    // we have to do this because Wnck has already reset wnck_window->app at this point
    XfwApplicationInstance *instance = g_hash_table_find(app->priv->instances, find_instance, window);

    g_signal_handlers_disconnect_by_data(window, app);

    app->priv->windows = g_list_remove(app->priv->windows, window);
    g_object_notify(G_OBJECT(app), "windows");

    instance->windows = g_list_remove(instance->windows, window);
    if (instance->windows == NULL) {
        g_hash_table_foreach_remove(app->priv->instances, find_instance, window);
        app->priv->instance_list = g_list_remove(app->priv->instance_list, instance);
        g_object_notify(G_OBJECT(app), "instances");
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

XfwApplicationX11 *
_xfw_application_x11_get(WnckClassGroup *wnck_group, XfwWindowX11 *window) {
    WnckApplication *wnck_app = wnck_window_get_application(_xfw_window_x11_get_wnck_window(window));
    XfwApplicationX11 *app = NULL;
    XfwApplicationInstance *instance;

    if (wnck_groups == NULL) {
        wnck_groups = g_hash_table_new(g_direct_hash, g_direct_equal);
    } else {
        app = g_hash_table_lookup(wnck_groups, wnck_group);
    }

    if (app == NULL) {
        app = g_object_new(XFW_TYPE_APPLICATION_X11,
                           "wnck-group", wnck_group,
                           NULL);
    } else {
        g_object_ref(app);
    }

    // avoid reference cycle
    g_object_add_toggle_ref(G_OBJECT(window), toggle_notify, app);
    g_object_weak_ref(G_OBJECT(app), weak_notify, window);

    app->priv->windows = g_list_prepend(app->priv->windows, window);
    g_signal_connect(window, "closed", G_CALLBACK(window_closed), app);
    g_object_notify(G_OBJECT(app), "windows");

    instance = g_hash_table_lookup(app->priv->instances, wnck_app);
    if (instance == NULL) {
        instance = g_new(XfwApplicationInstance, 1);
        instance->pid = wnck_application_get_pid(wnck_app);
        instance->name = g_strdup(wnck_application_get_name(wnck_app));
        instance->windows = g_list_prepend(NULL, window);
        g_hash_table_insert(app->priv->instances, g_object_ref(wnck_app), instance);
        app->priv->instance_list = g_list_prepend(app->priv->instance_list, instance);
        g_object_notify(G_OBJECT(app), "instances");
    } else {
        instance->windows = g_list_prepend(instance->windows, window);
    }

    return app;
}

const gchar *
_xfw_application_x11_get_icon_name(XfwApplicationX11 *app) {
    return app->priv->icon_name;
}
