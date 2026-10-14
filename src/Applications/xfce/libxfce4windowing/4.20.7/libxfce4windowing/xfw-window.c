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

/**
 * SECTION:xfw-window
 * @title: XfwWindow
 * @short_description: A representation of a toplevel window
 * @stability: Unstable
 * @include: libxfce4windowing/libxfce4windowing.h
 *
 * #XfwWindow describes a toplevel window on the screen, and provides
 * access to the window's state, type, and actions that can be performed
 * on it.
 *
 * Other metadata, like the window's title or icon, is also available.
 *
 * If the window supports it, actions can be taken, like minimizing,
 * maximizing, pinning, moving between workspaces, or closing the
 * window.
 *
 * Note that #XfwWindow is actually an interface; when obtaining an instance,
 * an instance of a windowing-environment-specific object that implements this
 * interface will be returned.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>
#include <stdint.h>

#include "libxfce4windowing-private.h"
#include "xfw-marshal.h"
#include "xfw-screen.h"
#include "xfw-window-private.h"
#include "libxfce4windowing-visibility.h"

#define XFW_WINDOW_GET_PRIVATE(window) ((XfwWindowPrivate *)xfw_window_get_instance_private(window))

enum {
    PROP_0,
    PROP_SCREEN,
    PROP_CLASS_IDS,
    PROP_NAME,
    PROP_TYPE,
    PROP_STATE,
    PROP_CAPABILITIES,
    PROP_WORKSPACE,
    PROP_MONITORS,
    PROP_APPLICATION,
    PROP_GICON,
};

typedef struct _XfwWindowPrivate {
    XfwScreen *screen;
    GIcon *gicon;

    GdkPixbuf *icon;
    gint icon_size;
    gint icon_scale;
} XfwWindowPrivate;

static void xfw_window_set_property(GObject *object,
                                    guint prop_id,
                                    const GValue *value,
                                    GParamSpec *pspec);
static void xfw_window_get_property(GObject *object,
                                    guint prop_id,
                                    GValue *value,
                                    GParamSpec *pspec);
static void xfw_window_finalize(GObject *object);


G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(XfwWindow, xfw_window, G_TYPE_OBJECT)

G_DEFINE_FLAGS_TYPE(
    XfwWindowState, xfw_window_state,
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_NONE, "none"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_ACTIVE, "active"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_MINIMIZED, "minimized"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_MAXIMIZED, "maximized"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_FULLSCREEN, "fullscreen"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_SKIP_PAGER, "skip-pager"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_SKIP_TASKLIST, "skip-tasklist"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_PINNED, "pinned"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_SHADED, "shaded"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_ABOVE, "above"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_BELOW, "below"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_STATE_URGENT, "urgent"))

G_DEFINE_FLAGS_TYPE(
    XfwWindowCapabilities, xfw_window_capabilities,
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_NONE, "none"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE, "can-minimize"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE, "can-unminimize"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE, "can-maximize"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE, "can-unmaximize"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN, "can-fullscreen"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN, "can-unfullscreen"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_SHADE, "can-shade"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_UNSHADE, "can-unshade"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_MOVE, "can-move"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_RESIZE, "can-resize"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE, "can-place-above"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE, "can-unplace-above"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW, "can-place-below"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW, "can-unplace-below"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE, "can-change-workspace"))

G_DEFINE_ENUM_TYPE(
    XfwWindowType, xfw_window_type,
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_NORMAL, "normal"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_DESKTOP, "desktop"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_DOCK, "dock"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_DIALOG, "dialog"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_TOOLBAR, "toolbar"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_MENU, "menu"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_UTILITY, "utility"),
    G_DEFINE_ENUM_VALUE(XFW_WINDOW_TYPE_SPLASHSCREEN, "splashscreen"))


static void
xfw_window_class_init(XfwWindowClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->set_property = xfw_window_set_property;
    gobject_class->get_property = xfw_window_get_property;
    gobject_class->finalize = xfw_window_finalize;

    /**
     * XfwWindow::class-changed:
     * @window: the object which received the signal.
     *
     * Emitted when at least one of the @window's class ids changes.
     *
     * Since: 4.19.3
     **/
    g_signal_new("class-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, class_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWindow::name-changed:
     * @window: the object which received the signal.
     *
     * Emitted when @window's name/title changes.
     **/
    g_signal_new("name-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, name_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWindow::icon-changed:
     * @window: the object which received the signal.
     *
     * Emitted when @window's icon changes.
     **/
    g_signal_new("icon-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, icon_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWindow::type-changed:
     * @window: the object which received the signal.
     * @old_type: the previous window type.
     *
     * Emitted when @window's type changes.
     **/
    g_signal_new("type-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, type_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__ENUM,
                 G_TYPE_NONE, 1,
                 XFW_TYPE_WINDOW_TYPE);

    /**
     * XfwWindow::state-changed:
     * @window: the object which received the signal.
     * @changed_mask: bitfield representing which state bits have changed.
     * @new_state: the new state bitfield.
     *
     * Emitted when @window's state changes.
     **/
    g_signal_new("state-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, state_changed),
                 NULL, NULL,
                 xfw_marshal_VOID__FLAGS_FLAGS,
                 G_TYPE_NONE, 2,
                 XFW_TYPE_WINDOW_STATE,
                 XFW_TYPE_WINDOW_STATE);

    /**
     * XfwWindow::capabilities-changed:
     * @window: the object which received the signal.
     * @changed_mask: bitfield representing which state bits have changed.
     * @new_state: the new state bitfield.
     *
     * Emitted when @window's capabilities change.
     **/
    g_signal_new("capabilities-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, capabilities_changed),
                 NULL, NULL,
                 xfw_marshal_VOID__FLAGS_FLAGS,
                 G_TYPE_NONE, 2,
                 XFW_TYPE_WINDOW_CAPABILITIES,
                 XFW_TYPE_WINDOW_CAPABILITIES);

    /**
     * XfwWindow::geometry-changed:
     * @window: the object which received the signal.
     *
     * Emitted when @window's position or size changes.
     **/
    g_signal_new("geometry-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, geometry_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWindow::workspace-changed:
     * @window: the object which received the signal.
     *
     * Emitted when @window is moved to a different worksapce.
     **/
    g_signal_new("workspace-changed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, state_changed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWindow::closed:
     * @window: the object which received the signal.
     *
     * Emitted when @window is closed.
     **/
    g_signal_new("closed",
                 XFW_TYPE_WINDOW,
                 G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(XfwWindowClass, closed),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE, 0);

    /**
     * XfwWindow:screen:
     *
     * The #XfwScreen instances that owns this window.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SCREEN,
                                    g_param_spec_object("screen",
                                                        "screen",
                                                        "screen",
                                                        XFW_TYPE_SCREEN,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:class-ids:
     *
     * The window's class ids.
     *
     * Since: 4.19.3
     */
    g_object_class_install_property(gobject_class,
                                    PROP_CLASS_IDS,
                                    g_param_spec_boxed("class-ids",
                                                       "class-ids",
                                                       "class-ids",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:name:
     *
     * The window's name or title.
     */
    g_object_class_install_property(gobject_class,
                                    PROP_NAME,
                                    g_param_spec_string("name",
                                                        "name",
                                                        "name",
                                                        "",
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:type
     *
     * The window's type or function.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_TYPE,
                                    g_param_spec_enum("type",
                                                      "type",
                                                      "type",
                                                      XFW_TYPE_WINDOW_TYPE,
                                                      XFW_WINDOW_TYPE_NORMAL,
                                                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:state:
     *
     * The window's state bitfield.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_STATE,
                                    g_param_spec_flags("state",
                                                       "state",
                                                       "state",
                                                       XFW_TYPE_WINDOW_STATE,
                                                       XFW_WINDOW_STATE_NONE,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:capabilities:
     *
     * The window's capabilities bitfield.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_CAPABILITIES,
                                    g_param_spec_flags("capabilities",
                                                       "capabilities",
                                                       "capabilities",
                                                       XFW_TYPE_WINDOW_CAPABILITIES,
                                                       XFW_WINDOW_CAPABILITIES_NONE,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:wokspace:
     *
     * The workspace the window is shown on.  May be %NULL if the window is not
     * on a workspace, or is pinned to all workspaces.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_WORKSPACE,
                                    g_param_spec_object("workspace",
                                                        "workspace",
                                                        "workspace",
                                                        XFW_TYPE_WORKSPACE,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:monitors: (type GList(XfwMonitor))
     *
     * The list of #XfwMonitor<!-- -->s (if any) that the window is displayed on.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_MONITORS,
                                    g_param_spec_pointer("monitors",
                                                         "monitors",
                                                         "monitors",
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:application:
     *
     * The #XfwApplication that owns this window.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_APPLICATION,
                                    g_param_spec_object("application",
                                                        "application",
                                                        "application",
                                                        XFW_TYPE_APPLICATION,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindow:gicon:
     *
     * The #GIcon that represents this window.
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
xfw_window_init(XfwWindow *window) {}

static void
xfw_window_set_property(GObject *object,
                        guint prop_id,
                        const GValue *value,
                        GParamSpec *pspec) {
    XfwWindowPrivate *priv = XFW_WINDOW_GET_PRIVATE(XFW_WINDOW(object));

    switch (prop_id) {
        case PROP_SCREEN:
            priv->screen = g_value_get_object(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
xfw_window_get_property(GObject *object,
                        guint prop_id,
                        GValue *value,
                        GParamSpec *pspec) {
    XfwWindow *window = XFW_WINDOW(object);
    XfwWindowPrivate *priv = XFW_WINDOW_GET_PRIVATE(window);

    switch (prop_id) {
        case PROP_SCREEN:
            g_value_set_object(value, priv->screen);
            break;

        case PROP_CLASS_IDS:
            g_value_set_boxed(value, xfw_window_get_class_ids(window));
            break;

        case PROP_NAME:
            g_value_set_string(value, xfw_window_get_name(window));
            break;

        case PROP_TYPE:
            g_value_set_enum(value, xfw_window_get_window_type(window));
            break;

        case PROP_STATE:
            g_value_set_flags(value, xfw_window_get_state(window));
            break;

        case PROP_CAPABILITIES:
            g_value_set_flags(value, xfw_window_get_capabilities(window));
            break;

        case PROP_WORKSPACE:
            g_value_set_object(value, xfw_window_get_workspace(window));
            break;

        case PROP_MONITORS:
            g_value_set_pointer(value, xfw_window_get_monitors(window));
            break;

        case PROP_APPLICATION:
            g_value_set_object(value, xfw_window_get_application(window));
            break;

        case PROP_GICON:
            g_value_set_object(value, priv->gicon);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
xfw_window_finalize(GObject *object) {
    XfwWindowPrivate *priv = XFW_WINDOW_GET_PRIVATE(XFW_WINDOW(object));

    g_clear_object(&priv->gicon);
    g_clear_object(&priv->icon);

    G_OBJECT_CLASS(xfw_window_parent_class)->finalize(object);
}

/**
 * xfw_window_get_class_ids:
 * @window: an #XfwWindow.
 *
 * Fetches @window's class ids. On X11 this should contain the class and instance
 * names of the [WM_CLASS property](https://x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html#wm_class_property).
 * On Wayland, it's likely to be limited to the [application ID](https://wayland.app/protocols/wlr-foreign-toplevel-management-unstable-v1#zwlr_foreign_toplevel_handle_v1:event:app_id),
 * which should correspond to the basename of the application's desktop file.
 *
 * Return value: (array zero-terminated=1) (element-type utf8) (transfer none):
 * a %NULL-terminated array of strings owned by the #XfwWindow.
 *
 * Since: 4.19.3
 **/
const gchar *const *
xfw_window_get_class_ids(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_class_ids)(window);
}

/**
 * xfw_window_get_name:
 * @window: an #XfwWindow.
 *
 * Fetches @window's name/title.
 *
 * Return value: (nullable) (transfer none): a window title, or %NULL if there
 * is no title.  The returned title should not be modified or freed.
 **/
const gchar *
xfw_window_get_name(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_name)(window);
}

/**
 * xfw_window_get_icon:
 * @window: an #XfwWindow.
 * @size: the desired icon size.
 * @scale: the UI scale factor.
 *
 * Fetches @window's icon.  If @window has no icon, a fallback icon may be
 * returned.  Whether or not the returned icon is a fallback icon can be
 * determined using #xfw_window_icon_is_fallback().
 *
 * Return value: (nullable) (transfer none): a #GdkPixbuf, owned by @window,
 * or %NULL if @window has no icon and a fallback cannot be rendered.
 **/
GdkPixbuf *
xfw_window_get_icon(XfwWindow *window, gint size, gint scale) {
    XfwWindowPrivate *priv;

    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);

    priv = XFW_WINDOW_GET_PRIVATE(window);
    if (priv->icon == NULL || priv->icon_size != size || priv->icon_scale != scale) {
        GIcon *gicon;

        if (priv->icon != NULL) {
            g_object_unref(priv->icon);
        }
        gicon = xfw_window_get_gicon(window);
        priv->icon = _xfw_gicon_load(gicon, size, scale);

        if (priv->icon != NULL) {
            priv->icon_size = size;
            priv->icon_scale = scale;
        }
    }

    return priv->icon;
}

/**
 * xfw_window_get_gicon:
 * @window: an #XfwWindow.
 *
 * Fetches @window's icon as a size-independent #GIcon.  If an icon cannot be
 * found, a #GIcon representing a fallback icon will be returned.  Whether or
 * not the returned icon is a fallback icon can be determined using
 * #xfw_window_icon_is_fallback().
 *
 * Return value: (not nullable) (transfer none): a #GIcon, owned by @window.
 *
 * Since: 4.19.1
 **/
GIcon *
xfw_window_get_gicon(XfwWindow *window) {
    XfwWindowPrivate *priv;

    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);

    priv = XFW_WINDOW_GET_PRIVATE(window);
    if (priv->gicon == NULL) {
        XfwWindowClass *klass = XFW_WINDOW_GET_CLASS(window);
        priv->gicon = klass->get_gicon(window);
    }

    return priv->gicon;
}

/**
 * xfw_window_icon_is_fallback:
 * @window: an #XfwWindow.
 *
 * Determines if @window does not have an icon, and thus a fallback icon
 * will be returned from #xfw_window_get_icon() and #xfw_window_get_gicon().
 *
 * Return value: %TRUE or %FALSE, depending on if @window's icon uses a
 * fallback icon or not.
 *
 * Since: 4.19.1
 **/
gboolean
xfw_window_icon_is_fallback(XfwWindow *window) {
    GIcon *gicon = xfw_window_get_gicon(window);

    if (G_IS_THEMED_ICON(gicon)) {
        return g_strv_contains(g_themed_icon_get_names(G_THEMED_ICON(gicon)),
                               XFW_WINDOW_FALLBACK_ICON_NAME);
    } else {
        return FALSE;
    }
}

/**
 * xfw_window_get_type:
 * @window: an #XfwWindow.
 *
 * Fetches @window's type/function.
 *
 * Return value: a member of #XfwWindowType.
 **/
XfwWindowType
xfw_window_get_window_type(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), XFW_WINDOW_TYPE_NORMAL);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_window_type)(window);
}

/**
 * xfw_window_get_state:
 * @window: an #XfwWindow.
 *
 * Fetches @window's state bitfield.
 *
 * Return value: a bitfield with zero or more bits from #XfwWindowState set.
 **/
XfwWindowState
xfw_window_get_state(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), XFW_WINDOW_STATE_NONE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_state)(window);
}

/**
 * xfw_window_get_capabilities:
 * @window: an #XfwWindow.
 *
 * Fetches @window's capabilities bitfield.
 *
 * Return value: a bitfield with zero or more bits from #XfwWindowCapabilities
 * set.
 **/
XfwWindowCapabilities
xfw_window_get_capabilities(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), XFW_WINDOW_CAPABILITIES_NONE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_capabilities)(window);
}

/**
 * xfw_window_get_geometry:
 * @window: an #XfwWindow.
 *
 * Fetches @window's position and size.
 *
 * Return value: (not nullable) (transfer none): A #GdkRectangle representing
 * @window's geometry, which should not be modified or freed.
 **/
GdkRectangle *
xfw_window_get_geometry(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_geometry)(window);
}

/**
 * xfw_window_get_screen:
 * @window: an #XfwWindow.
 *
 * Fetches the #XfwScreen instance that owns @window.
 *
 * Return value: (not nullable) (transfer none): A #XfwScreen instance, with a
 * reference owned by @window.
 **/
XfwScreen *
xfw_window_get_screen(XfwWindow *window) {
    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);
    return XFW_WINDOW_GET_PRIVATE(window)->screen;
}

/**
 * xfw_window_get_workspace:
 * @window: an #XfwWindow.
 *
 * Fetches @window's workspace, if any.  This may return %NULL if @window is
 * not on a workspace, or is pinned to all workspaces.
 *
 * Return value: (nullable) (transfer none): A #XfwWorkspace instance, with a
 * reference owned by @window, or %NULL.
 **/
XfwWorkspace *
xfw_window_get_workspace(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_workspace)(window);
}

/**
 * xfw_window_get_monitors:
 * @window: an #XfwWindow.
 *
 * Fetches the list of monitors @window is displayed on, if any.
 *
 * Return value: (nullable) (element-type XfwMonitor) (transfer none): A list
 * of #XfwMonitor instances, or %NULL.  The list and its contents are owned by
 * @window and should not be modified or freed.
 **/
GList *
xfw_window_get_monitors(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_monitors)(window);
}

/**
 * xfw_window_get_application:
 * @window: an #XfwWindow.
 *
 * Fetches @window's application.
 *
 * Return value: (transfer none): An #XfwApplication instance, with a
 * reference owned by @window.
 **/
XfwApplication *
xfw_window_get_application(XfwWindow *window) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), NULL);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->get_application)(window);
}

gboolean
xfw_window_activate(XfwWindow *window, XfwSeat *seat, guint64 event_timestamp, GError **error) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->activate)(window, seat, event_timestamp, error);
}

gboolean
xfw_window_close(XfwWindow *window, guint64 event_timestamp, GError **error) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->close)(window, event_timestamp, error);
}

gboolean
xfw_window_start_move(XfwWindow *window, GError **error) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->start_move)(window, error);
}

gboolean
xfw_window_start_resize(XfwWindow *window, GError **error) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->start_resize)(window, error);
}

gboolean
xfw_window_set_geometry(XfwWindow *window, const GdkRectangle *rect, GError **error) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->set_geometry)(window, rect, error);
}

gboolean
xfw_window_set_button_geometry(XfwWindow *window, GdkWindow *relative_to, const GdkRectangle *rect, GError **error) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->set_button_geometry)(window, relative_to, rect, error);
}

gboolean
xfw_window_move_to_workspace(XfwWindow *window, XfwWorkspace *workspace, GError **error) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->move_to_workspace)(window, workspace, error);
}

#define STATE_SETTER(state) \
    gboolean \
        xfw_window_set_##state(XfwWindow *window, gboolean is_##state, GError **error) { \
        XfwWindowClass *klass; \
        g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE); \
        klass = XFW_WINDOW_GET_CLASS(window); \
        return (*klass->set_##state)(window, is_##state, error); \
    }

STATE_SETTER(minimized)
STATE_SETTER(maximized)
STATE_SETTER(fullscreen)
STATE_SETTER(skip_pager)
STATE_SETTER(skip_tasklist)
STATE_SETTER(pinned)
STATE_SETTER(shaded)
STATE_SETTER(above)
STATE_SETTER(below)

#undef STATE_SETTER

#define STATE_GETTER(state_lower, state_upper) \
    gboolean \
        xfw_window_is_##state_lower(XfwWindow *window) { \
        XfwWindowState state; \
        g_return_val_if_fail(XFW_IS_WINDOW(window), XFW_WINDOW_STATE_NONE); \
        state = xfw_window_get_state(window); \
        return (state & XFW_WINDOW_STATE_##state_upper) != 0; \
    }

STATE_GETTER(active, ACTIVE)
STATE_GETTER(minimized, MINIMIZED)
STATE_GETTER(maximized, MAXIMIZED)
STATE_GETTER(fullscreen, FULLSCREEN)
STATE_GETTER(skip_pager, SKIP_PAGER)
STATE_GETTER(skip_tasklist, SKIP_TASKLIST)
STATE_GETTER(pinned, PINNED)
STATE_GETTER(shaded, SHADED)
STATE_GETTER(above, ABOVE)
STATE_GETTER(below, BELOW)
STATE_GETTER(urgent, URGENT)

#undef STATE_GETTER

gboolean
xfw_window_is_on_workspace(XfwWindow *window, XfwWorkspace *workspace) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->is_on_workspace)(window, workspace);
}

gboolean
xfw_window_is_in_viewport(XfwWindow *window, XfwWorkspace *workspace) {
    XfwWindowClass *klass;
    g_return_val_if_fail(XFW_IS_WINDOW(window), FALSE);
    klass = XFW_WINDOW_GET_CLASS(window);
    return (*klass->is_in_viewport)(window, workspace);
}

XfwScreen *
_xfw_window_get_screen(XfwWindow *window) {
    return XFW_WINDOW_GET_PRIVATE(window)->screen;
}

void
_xfw_window_invalidate_icon(XfwWindow *window) {
    XfwWindowPrivate *priv = XFW_WINDOW_GET_PRIVATE(window);

    g_clear_object(&priv->icon);
    g_clear_object(&priv->gicon);
    priv->icon_size = 0;
    priv->icon_scale = 0;
}

#define __XFW_WINDOW_C__
#include "libxfce4windowing-visibility.c"
