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

/**
 * SECTION:xfw-application
 * @title: XfwApplication
 * @short_description: An object representing a desktop application
 * @stability: Unstable
 * @include: libxfce4windowing/libxfce4windowing.h
 *
 * #XfwApplication represents an application in the common or abstract sense,
 * i.e. it can have several windows belonging to different instances, identified
 * by their process ID.
 *
 * Note that #XfwApplication is actually an interface; when obtaining an
 * instance, an instance of a windowing-environment-specific object that
 * implements this interface will be returned.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "libxfce4windowing-private.h"
#include "xfw-application-private.h"
#include "libxfce4windowing-visibility.h"

#define XFW_APPLICATION_GET_PRIVATE(app) ((XfwApplicationPrivate *)xfw_application_get_instance_private(XFW_APPLICATION(app)))

enum {
    PROP_0,
    PROP_CLASS_ID,
    PROP_NAME,
    PROP_WINDOWS,
    PROP_INSTANCES,
    PROP_GICON,
};

typedef struct _XfwApplicationPrivate {
    GIcon *gicon;

    GdkPixbuf *icon;
    gint icon_size;
    gint icon_scale;
} XfwApplicationPrivate;


static void xfw_application_set_property(GObject *object,
                                         guint prop_id,
                                         const GValue *value,
                                         GParamSpec *pspec);
static void xfw_application_get_property(GObject *object,
                                         guint prop_id,
                                         GValue *value,
                                         GParamSpec *pspec);
static void xfw_application_finalize(GObject *object);


G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(XfwApplication, xfw_application, G_TYPE_OBJECT)


static void
xfw_application_class_init(XfwApplicationClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->set_property = xfw_application_set_property;
    gobject_class->get_property = xfw_application_get_property;
    gobject_class->finalize = xfw_application_finalize;

    /**
     * XfwApplication::icon-changed:
     * @app: the object which received the signal.
     *
     * Emitted when @app's icon changes.
     **/
    g_signal_new("icon-changed",
                 XFW_TYPE_APPLICATION,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwApplicationClass, icon_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwApplication:class-id:
     *
     * The application class id.
     *
     * Since: 4.19.3
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_CLASS_ID,
                                    g_param_spec_string("class-id",
                                                        "class-id",
                                                        "class-id",
                                                        "",
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwApplication:name:
     *
     * The application name.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_NAME,
                                    g_param_spec_string("name",
                                                        "name",
                                                        "name",
                                                        NULL,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwApplication:windows: (type GList(XfwWindow))
     *
     * The list of #XfwWindow belonging to the application.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_WINDOWS,
                                    g_param_spec_pointer("windows",
                                                         "windows",
                                                         "windows",
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwApplication:instances: (type GList(XfwApplicationInstance))
     *
     * The list of #XfwApplicationInstance belonging to the application.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_INSTANCES,
                                    g_param_spec_pointer("instances",
                                                         "instances",
                                                         "instances",
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwApplication:gicon:
     *
     * The #GIcon that represents this application.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_GICON,
                                    g_param_spec_object("gicon",
                                                        "gicon",
                                                        "gicon",
                                                        G_TYPE_ICON,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void
xfw_application_init(XfwApplication *app) {}

static void
xfw_application_set_property(GObject *object,
                             guint prop_id,
                             const GValue *value,
                             GParamSpec *pspec) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void
xfw_application_get_property(GObject *object,
                             guint prop_id,
                             GValue *value,
                             GParamSpec *pspec) {
    XfwApplication *app = XFW_APPLICATION(object);

    switch (prop_id) {
        case PROP_CLASS_ID:
            g_value_set_string(value, xfw_application_get_class_id(app));
            break;

        case PROP_NAME:
            g_value_set_string(value, xfw_application_get_name(app));
            break;

        case PROP_WINDOWS:
            g_value_set_pointer(value, xfw_application_get_windows(app));
            break;

        case PROP_INSTANCES:
            g_value_set_pointer(value, xfw_application_get_instances(app));
            break;

        case PROP_GICON:
            g_value_set_object(value, xfw_application_get_gicon(app));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
xfw_application_finalize(GObject *object) {
    XfwApplicationPrivate *priv = XFW_APPLICATION_GET_PRIVATE(object);

    g_clear_object(&priv->gicon);
    g_clear_object(&priv->icon);

    G_OBJECT_CLASS(xfw_application_parent_class)->finalize(object);
}

/**
 * xfw_application_get_class_id:
 * @app: an #XfwApplication.
 *
 * Fetches this application's class id. On X11 this should be the class name of
 * the [WM_CLASS property](https://x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html#wm_class_property).
 * On Wayland, it's the [application ID](https://wayland.app/protocols/wlr-foreign-toplevel-management-unstable-v1#zwlr_foreign_toplevel_handle_v1:event:app_id),
 * which should correspond to the basename of the application's desktop file.
 *
 * Return value: (not nullable) (transfer none): A UTF-8 formatted string,
 * owned by @app.
 *
 * Since: 4.19.3
 **/
const gchar *
xfw_application_get_class_id(XfwApplication *app) {
    XfwApplicationClass *klass;
    g_return_val_if_fail(XFW_IS_APPLICATION(app), NULL);
    klass = XFW_APPLICATION_GET_CLASS(app);
    return (*klass->get_class_id)(app);
}

/**
 * xfw_application_get_name:
 * @app: an #XfwApplication.
 *
 * Fetches this application's human-readable name.
 *
 * Return value: (not nullable) (transfer none): A UTF-8 formatted string,
 * owned by @app.
 **/
const gchar *
xfw_application_get_name(XfwApplication *app) {
    XfwApplicationClass *klass;
    g_return_val_if_fail(XFW_IS_APPLICATION(app), NULL);
    klass = XFW_APPLICATION_GET_CLASS(app);
    return (*klass->get_name)(app);
}

/**
 * xfw_application_get_icon:
 * @app: an #XfwApplication.
 * @size: the desired icon size.
 * @scale: the UI scale factor.
 *
 * Fetches @app's icon.  If @app has no icon, a fallback icon may be
 * returned.  Whether or not the returned icon is a fallback icon can be
 * determined using #xfw_application_icon_is_fallback().
 *
 * Return value: (nullable) (transfer none): a #GdkPixbuf, owned by @app,
 * or %NULL if @app has no icon and a fallback cannot be rendered.
 **/
GdkPixbuf *
xfw_application_get_icon(XfwApplication *app, gint size, gint scale) {
    XfwApplicationPrivate *priv;

    g_return_val_if_fail(XFW_IS_APPLICATION(app), NULL);

    priv = XFW_APPLICATION_GET_PRIVATE(app);
    if (priv->icon == NULL || priv->icon_size != size || priv->icon_scale != scale) {
        GIcon *gicon;

        if (priv->icon != NULL) {
            g_object_unref(priv->icon);
        }
        gicon = xfw_application_get_gicon(app);
        priv->icon = _xfw_gicon_load(gicon, size, scale);

        if (priv->icon != NULL) {
            priv->icon_size = size;
            priv->icon_scale = scale;
        }
    }

    return priv->icon;
}

/**
 * xfw_application_get_gicon:
 * @app: an #XfwApplication.
 *
 * Fetches @app's icon as a size-independent #GIcon.  If an icon cannot be
 * found, a #GIcon representing a fallback icon will be returned.  Whether or
 * not the returned icon is a fallback icon can be determined using
 * #xfw_application_icon_is_fallback().
 *
 * Return value: (not nullable) (transfer none): a #GIcon, owned by @app.
 *
 * Since: 4.19.1
 **/
GIcon *
xfw_application_get_gicon(XfwApplication *app) {
    XfwApplicationPrivate *priv;

    g_return_val_if_fail(XFW_IS_APPLICATION(app), NULL);

    priv = XFW_APPLICATION_GET_PRIVATE(app);
    if (priv->gicon == NULL) {
        XfwApplicationClass *klass = XFW_APPLICATION_GET_CLASS(app);
        priv->gicon = klass->get_gicon(app);
    }

    return priv->gicon;
}

/**
 * xfw_application_icon_is_fallback:
 * @app: an #XfwApplication.
 *
 * Determines if @app does not have an icon, and thus a fallback icon
 * will be returned from #xfw_application_get_icon() and
 * #xfw_application_get_gicon().
 *
 * Return value: %TRUE or %FALSE, depending on if @app's icon uses a
 * fallback icon or not.
 *
 * Since: 4.19.1
 **/
gboolean
xfw_application_icon_is_fallback(XfwApplication *app) {
    GIcon *gicon = xfw_application_get_gicon(app);

    if (G_IS_THEMED_ICON(gicon)) {
        return g_strv_contains(g_themed_icon_get_names(G_THEMED_ICON(gicon)),
                               XFW_APPLICATION_FALLBACK_ICON_NAME);
    } else {
        return FALSE;
    }
}

/**
 * xfw_application_get_windows:
 * @app: an #XfwApplication.
 *
 * Lists all windows belonging to the application.
 *
 * Return value: (not nullable) (element-type XfwWindow) (transfer none):
 * The list of #XfwWindow belonging to @app. The list and its contents are owned
 * by @app.
 **/
GList *
xfw_application_get_windows(XfwApplication *app) {
    XfwApplicationClass *klass;
    g_return_val_if_fail(XFW_IS_APPLICATION(app), NULL);
    klass = XFW_APPLICATION_GET_CLASS(app);
    return (*klass->get_windows)(app);
}

/**
 * xfw_application_get_instances:
 * @app: an #XfwApplication.
 *
 * Lists all instances of the application.
 *
 * Return value: (nullable) (element-type XfwApplicationInstance) (transfer none):
 * The list of #XfwApplicationInstance of @app, or %NULL if listing instances is
 * not supported on the windowing environment in use. The list and its contents
 * are owned by @app.
 **/
GList *
xfw_application_get_instances(XfwApplication *app) {
    XfwApplicationClass *klass;
    g_return_val_if_fail(XFW_IS_APPLICATION(app), NULL);
    klass = XFW_APPLICATION_GET_CLASS(app);
    return (*klass->get_instances)(app);
}

/**
 * xfw_application_get_instance:
 * @app: an #XfwApplication.
 * @window: the application window you want to get the instance of.
 *
 * Finds the #XfwApplicationInstance to which @window belongs.
 *
 * Return value: (nullable) (transfer none):
 * The #XfwApplicationInstance to which @window belongs, or %NULL if @window
 * does not belong to @app, or if listing instances is not supported on the
 * windowing environment in use. The returned #XfwApplicationInstance is owned
 * by @app.
 **/
XfwApplicationInstance *
xfw_application_get_instance(XfwApplication *app, XfwWindow *window) {
    XfwApplicationClass *klass;
    g_return_val_if_fail(XFW_IS_APPLICATION(app), NULL);
    klass = XFW_APPLICATION_GET_CLASS(app);
    return (*klass->get_instance)(app, window);
}

/**
 * xfw_application_instance_get_pid:
 * @instance: an #XfwApplicationInstance.
 *
 * Fetches @instance's PID.
 *
 * Return value: The process ID of @instance, or 0 if none is available.
 *
 * Since: 4.19.1
 **/
gint
xfw_application_instance_get_pid(XfwApplicationInstance *instance) {
    g_return_val_if_fail(instance != NULL, 0);
    return instance->pid;
}

/**
 * xfw_application_instance_get_name:
 * @instance: an #XfwApplicationInstance.
 *
 * Fetches @instance's name, which can often be the same as the application name.
 *
 * Return value: (not nullable) (transfer none): A string owned by @instance.
 *
 * Since: 4.19.1
 **/
const gchar *
xfw_application_instance_get_name(XfwApplicationInstance *instance) {
    g_return_val_if_fail(instance != NULL, NULL);
    return instance->name;
}

/**
 * xfw_application_instance_get_windows:
 * @instance: an #XfwApplicationInstance.
 *
 * Lists all windows belonging to the application instance.
 *
 * Return value: (not nullable) (element-type XfwWindow) (transfer none):
 * The list of #XfwWindow belonging to @instance. The list and its contents are owned
 * by @instance.
 *
 * Since: 4.19.1
 **/
GList *
xfw_application_instance_get_windows(XfwApplicationInstance *instance) {
    g_return_val_if_fail(instance != NULL, NULL);
    return instance->windows;
}

void
_xfw_application_invalidate_icon(XfwApplication *app) {
    XfwApplicationPrivate *priv = XFW_APPLICATION_GET_PRIVATE(app);

    g_clear_object(&priv->icon);
    g_clear_object(&priv->gicon);
    priv->icon_size = 0;
    priv->icon_scale = 0;
}

void
_xfw_application_instance_free(gpointer data) {
    XfwApplicationInstance *instance = data;
    g_free(instance->name);
    g_list_free(instance->windows);
    g_free(instance);
}

#define __XFW_APPLICATION_C__
#include "libxfce4windowing-visibility.c"
