/*
 * Copyright (c) 2023 Brian Tarricone <brian@tarricone.org>
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

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <cairo-xlib.h>
#include <glib/gi18n-lib.h>

#include "libxfce4windowing-private.h"
#include "window-icon-utils.h"
#include "xfw-util.h"
#include "xfw-wnck-icon.h"

enum {
    PROP_0,
    PROP_WNCK_OBJECT,
};

struct _XfwWnckIcon {
    GObject parent;

    GObject *wnck_object;

    GList *window_icons;
};

struct _XfwWnckIconClass {
    GObjectClass parent_class;
};

static void xfw_wnck_icon_set_property(GObject *object,
                                       guint prop_id,
                                       const GValue *value,
                                       GParamSpec *pspec);
static void xfw_wnck_icon_get_property(GObject *object,
                                       guint prop_id,
                                       GValue *value,
                                       GParamSpec *pspec);
static void xfw_wnck_icon_dispose(GObject *object);
static void xfw_wnck_icon_finalize(GObject *object);

static void xfw_wnck_icon_initable_init(GInitableIface *iface);
static void xfw_wnck_icon_gicon_init(GIconIface *iface);
static void xfw_wnck_icon_loadable_icon_init(GLoadableIconIface *iface);

static gboolean xfw_wnck_icon_initable_real_init(GInitable *initable,
                                                 GCancellable *cancellable,
                                                 GError **error);

static gboolean xfw_wnck_icon_equal(GIcon *icon1,
                                    GIcon *icon2);
static guint xfw_wnck_icon_hash(GIcon *icon);

static GInputStream *xfw_wnck_icon_load(GLoadableIcon *icon,
                                        int size,
                                        char **type,
                                        GCancellable *cancellable,
                                        GError **error);
static void xfw_wnck_icon_load_async(GLoadableIcon *icon,
                                     int size,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data);
static GInputStream *xfw_wnck_icon_load_finish(GLoadableIcon *icon,
                                               GAsyncResult *res,
                                               char **type,
                                               GError **error);

static GList *xfw_wnck_object_get_net_wm_icon(GObject *wnck_object);
static WindowIcon *xfw_wnck_object_get_wmhints_icon(GObject *wnck_object);

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWnckIcon,
                              xfw_wnck_icon,
                              G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, xfw_wnck_icon_initable_init)
                              G_IMPLEMENT_INTERFACE(G_TYPE_ICON, xfw_wnck_icon_gicon_init)
                              G_IMPLEMENT_INTERFACE(G_TYPE_LOADABLE_ICON, xfw_wnck_icon_loadable_icon_init))

static void
xfw_wnck_icon_class_init(XfwWnckIconClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->set_property = xfw_wnck_icon_set_property;
    gobject_class->get_property = xfw_wnck_icon_get_property;
    gobject_class->dispose = xfw_wnck_icon_dispose;
    gobject_class->finalize = xfw_wnck_icon_finalize;

    g_object_class_install_property(gobject_class,
                                    PROP_WNCK_OBJECT,
                                    g_param_spec_object("wnck-object",
                                                        "wnck-object",
                                                        "Either a WnckWindow or WnckClassGroup",
                                                        G_TYPE_OBJECT,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_wnck_icon_set_property(GObject *object,
                           guint prop_id,
                           const GValue *value,
                           GParamSpec *pspec) {
    XfwWnckIcon *icon = XFW_WNCK_ICON(object);

    switch (prop_id) {
        case PROP_WNCK_OBJECT:
            icon->wnck_object = g_value_dup_object(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
xfw_wnck_icon_get_property(GObject *object,
                           guint prop_id,
                           GValue *value,
                           GParamSpec *pspec) {
    XfwWnckIcon *icon = XFW_WNCK_ICON(object);

    switch (prop_id) {
        case PROP_WNCK_OBJECT:
            g_value_set_object(value, icon->wnck_object);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
xfw_wnck_icon_dispose(GObject *object) {
    XfwWnckIcon *icon = XFW_WNCK_ICON(object);

    g_clear_object(&icon->wnck_object);

    G_OBJECT_CLASS(xfw_wnck_icon_parent_class)->dispose(object);
}

static void
xfw_wnck_icon_finalize(GObject *object) {
    XfwWnckIcon *icon = XFW_WNCK_ICON(object);

    g_list_free_full(icon->window_icons, (GDestroyNotify)_window_icon_free);

    G_OBJECT_CLASS(xfw_wnck_icon_parent_class)->finalize(object);
}

static void
xfw_wnck_icon_initable_init(GInitableIface *iface) {
    iface->init = xfw_wnck_icon_initable_real_init;
}

static void
xfw_wnck_icon_gicon_init(GIconIface *iface) {
    iface->equal = xfw_wnck_icon_equal;
    iface->hash = xfw_wnck_icon_hash;
}

static void
xfw_wnck_icon_loadable_icon_init(GLoadableIconIface *iface) {
    iface->load = xfw_wnck_icon_load;
    iface->load_async = xfw_wnck_icon_load_async;
    iface->load_finish = xfw_wnck_icon_load_finish;
}

static void
xfw_wnck_icon_init(XfwWnckIcon *icon) {}

static gboolean
xfw_wnck_icon_initable_real_init(GInitable *initable,
                                 GCancellable *cancellable,
                                 GError **error) {
    XfwWnckIcon *icon = XFW_WNCK_ICON(initable);
    GObject *wnck_object = icon->wnck_object;
    GList *window_icons = NULL;

    g_return_val_if_fail(WNCK_IS_WINDOW(wnck_object) || WNCK_IS_CLASS_GROUP(wnck_object), FALSE);

    window_icons = xfw_wnck_object_get_net_wm_icon(wnck_object);
    if (G_UNLIKELY(window_icons == NULL)) {
        WindowIcon *wmhints_icon = xfw_wnck_object_get_wmhints_icon(wnck_object);
        if (wmhints_icon != NULL) {
            window_icons = g_list_prepend(window_icons, wmhints_icon);
        }
    }

    if (G_LIKELY(window_icons != NULL)) {
        icon->window_icons = window_icons;
        return TRUE;
    } else {
        if (error != NULL) {
            *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("The provided window does not have a _NET_WM_ICON or WMHints icon"));
        }
        return FALSE;
    }
}

static gboolean
xfw_wnck_icon_equal(GIcon *icon1,
                    GIcon *icon2) {
    GObject *wnck_object1, *wnck_object2;

    if (!XFW_IS_WNCK_ICON(icon1) || !XFW_IS_WNCK_ICON(icon2)) {
        return FALSE;
    }

    wnck_object1 = XFW_WNCK_ICON(icon1)->wnck_object;
    wnck_object2 = XFW_WNCK_ICON(icon2)->wnck_object;

    if (WNCK_IS_WINDOW(wnck_object1) && WNCK_IS_WINDOW(wnck_object2)) {
        return wnck_window_get_xid(WNCK_WINDOW(wnck_object1)) == wnck_window_get_xid(WNCK_WINDOW(wnck_object2));
    } else if (WNCK_IS_CLASS_GROUP(wnck_object1) && WNCK_IS_CLASS_GROUP(wnck_object2)) {
        return g_strcmp0(wnck_class_group_get_id(WNCK_CLASS_GROUP(wnck_object1)), wnck_class_group_get_id(WNCK_CLASS_GROUP(wnck_object2))) == 0;
    } else {
        return FALSE;
    }
}

static guint
xfw_wnck_icon_hash(GIcon *icon) {
    XfwWnckIcon *wnck_icon = XFW_WNCK_ICON(icon);

    if (WNCK_IS_WINDOW(wnck_icon->wnck_object)) {
        return wnck_window_get_xid(WNCK_WINDOW(wnck_icon->wnck_object));
    } else if (WNCK_IS_CLASS_GROUP(wnck_icon->wnck_object)) {
        return g_str_hash(wnck_class_group_get_id(WNCK_CLASS_GROUP(wnck_icon->wnck_object)));
    } else {
        g_warn_if_reached();
        return 0;
    }
}

static GList *
xfw_wnck_object_get_net_wm_icon(GObject *wnck_object) {
    GdkDisplay *display;
    Display *dpy;
    Window xid;
    gint res, err;
    Atom type = None;
    gint format = 0;
    gulong nitems = 0;
    gulong bytes_after = 0;
    gulong *data = NULL;
    GList *window_icons = NULL;

    g_return_val_if_fail(WNCK_IS_WINDOW(wnck_object) || WNCK_IS_CLASS_GROUP(wnck_object), NULL);

    display = gdk_display_get_default();
    dpy = gdk_x11_display_get_xdisplay(display);

    xid = _xfw_wnck_object_get_x11_window(wnck_object);
    if (xid == None) {
        return NULL;
    }

    xfw_windowing_error_trap_push(display);

    res = XGetWindowProperty(dpy,
                             xid,
                             XInternAtom(dpy, "_NET_WM_ICON", False),
                             0, G_MAXLONG,
                             False,
                             XA_CARDINAL,
                             &type, &format, &nitems, &bytes_after, (void *)&data);

    err = xfw_windowing_error_trap_pop(display);

    if (err == Success && res == Success && type == XA_CARDINAL && format == 32 && data != NULL) {
        gulong *cur = data;

        while (cur + 2 < data + nitems) {
            WindowIcon *window_icon = NULL;
            gint width = cur[0];
            gint height = cur[1];

            if (width <= 0 || height <= 0) {
                g_message("Invalid _NET_WM_ICON dimensions %dx%d for icon for window %lu", width, height, xid);
                break;
            } else if (cur + 2 + (width * height) > data + nitems) {
                break;
            }

            // Xlib returns format-32 properties as long[], so narrow each pixel to packed 32-bit.
            guint32 *argb32 = g_new(guint32, width * height);
            for (gint i = 0; i < width * height; ++i) {
                argb32[i] = cur[2 + i];
            }
            window_icon = _window_icon_new(argb32, width, height, FALSE);
            g_free(argb32);
            if (G_LIKELY(window_icon != NULL)) {
                window_icons = g_list_prepend(window_icons, window_icon);
            }

            cur += 2 + (width * height);
        }
    }

    if (data != NULL) {
        XFree(data);
    }

    return g_list_sort(window_icons, _window_icon_compare);
}

static cairo_surface_t *
xfw_cairo_surface_from_drawable(Drawable drawable,
                                guint *width_out,
                                guint *height_out) {
    cairo_surface_t *surface = NULL;
    GdkDisplay *display;
    Display *dpy;
    Window root;
    int err, result;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, border_width, depth;
    Visual *visual;

    g_return_val_if_fail(drawable != None, NULL);

    display = gdk_display_get_default();
    dpy = gdk_x11_display_get_xdisplay(display);
    visual = gdk_x11_visual_get_xvisual(gdk_screen_get_system_visual(gdk_screen_get_default()));

    xfw_windowing_error_trap_push(display);
    result = XGetGeometry(dpy, drawable, &root, &x, &y, &width, &height, &border_width, &depth);
    err = xfw_windowing_error_trap_pop(display);

    if (result != Success || err != Success) {
        return NULL;
    }

    surface = cairo_xlib_surface_create(dpy, drawable, visual, width, height);
    if (G_LIKELY(surface != NULL)) {
        if (width_out != NULL) {
            *width_out = width;
        }
        if (height_out != NULL) {
            *height_out = height;
        }
    }

    return surface;
}

static cairo_surface_t *
xfw_cairo_surface_from_pixmap_and_mask(Pixmap pixmap,
                                       Pixmap mask) {
    cairo_surface_t *surface = NULL;
    cairo_surface_t *pix_surface, *mask_surface = NULL;
    GdkDisplay *display;
    guint width = 0, height = 0;

    g_return_val_if_fail(pixmap != None, NULL);

    display = gdk_display_get_default();

    pix_surface = xfw_cairo_surface_from_drawable(pixmap, &width, &height);
    if (G_LIKELY(pix_surface != NULL)) {
        cairo_t *cr;
        int err;

        if (mask != None) {
            mask_surface = xfw_cairo_surface_from_drawable(mask, NULL, NULL);
        }

        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        cr = cairo_create(surface);

        xfw_windowing_error_trap_push(display);

        if (cairo_surface_get_content(pix_surface) == CAIRO_CONTENT_ALPHA) {
            // for alpha-only pixmaps, we need to treat it as a bitmap,
            // where transparent = black and opaque = white
            cairo_push_group(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_paint(cr);
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_mask_surface(cr, pix_surface, 0, 0);
            cairo_pop_group_to_source(cr);
        } else {
            cairo_set_source_surface(cr, pix_surface, 0, 0);
        }

        if (mask_surface != NULL) {
            cairo_mask_surface(cr, mask_surface, 0, 0);
        } else {
            cairo_paint(cr);
        }

        cairo_surface_destroy(pix_surface);
        if (mask_surface != NULL) {
            cairo_surface_destroy(mask_surface);
        }

        err = xfw_windowing_error_trap_pop(display);
        if (err != Success) {
            cairo_surface_destroy(surface);
            surface = NULL;
        }
    }

    return surface;
}

static WindowIcon *
xfw_wnck_object_get_wmhints_icon(GObject *wnck_object) {
    WindowIcon *window_icon = NULL;
    GdkDisplay *display;
    Display *dpy;
    Window xid;
    XWMHints *hints;
    int err;

    g_return_val_if_fail(WNCK_IS_WINDOW(wnck_object) || WNCK_IS_CLASS_GROUP(wnck_object), NULL);

    display = gdk_display_get_default();
    dpy = gdk_x11_display_get_xdisplay(display);

    xid = _xfw_wnck_object_get_x11_window(wnck_object);
    if (xid == None) {
        return NULL;
    }

    xfw_windowing_error_trap_push(display);
    hints = XGetWMHints(dpy, xid);
    err = xfw_windowing_error_trap_pop(display);

    if (hints != NULL && err == Success) {
        if ((hints->flags & IconPixmapHint) != 0) {
            Pixmap mask = (hints->flags & IconMaskHint) != 0 ? hints->icon_mask : None;
            cairo_surface_t *surface = xfw_cairo_surface_from_pixmap_and_mask(hints->icon_pixmap, mask);

            if (surface != NULL) {
                window_icon = _window_icon_new((guint32 *)(gpointer)cairo_image_surface_get_data(surface),
                                               cairo_image_surface_get_width(surface),
                                               cairo_image_surface_get_height(surface),
                                               FALSE);
                cairo_surface_destroy(surface);
            }
        }
    }

    if (hints != NULL) {
        XFree(hints);
    }

    return window_icon;
}

static GInputStream *
xfw_wnck_icon_load(GLoadableIcon *icon,
                   int size,
                   char **type,
                   GCancellable *cancellable,
                   GError **error) {
    XfwWnckIcon *wnck_icon = XFW_WNCK_ICON(icon);
    WindowIcon *window_icon = NULL;

    if (wnck_icon->window_icons == NULL) {
        wnck_icon->window_icons = xfw_wnck_object_get_net_wm_icon(wnck_icon->wnck_object);
    }

    if (G_LIKELY(wnck_icon->window_icons != NULL)) {
        for (GList *l = wnck_icon->window_icons; l != NULL; l = l->next) {
            WindowIcon *wi = l->data;
            if (MAX(wi->width, wi->height) >= size) {
                window_icon = wi;
                break;
            }
        }

        if (G_UNLIKELY(window_icon == NULL)) {
            window_icon = g_list_last(wnck_icon->window_icons)->data;
        }
    }

    if (G_LIKELY(window_icon != NULL)) {
        guchar *bmp = g_memdup2(window_icon->bmp, window_icon->bmp_len);
        return g_memory_input_stream_new_from_data(bmp, window_icon->bmp_len, g_free);
    } else {
        if (error != NULL) {
            *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("Failed to find or load an icon for the window"));
        }
        return NULL;
    }
}

static void
xfw_wnck_icon_load_async(GLoadableIcon *icon,
                         int size,
                         GCancellable *cancellable,
                         GAsyncReadyCallback callback,
                         gpointer user_data) {
    GInputStream *stream;
    GTask *task = g_task_new(icon, cancellable, callback, user_data);
    gchar *type = NULL;
    GError *error = NULL;

    stream = xfw_wnck_icon_load(icon, size, &type, cancellable, &error);
    if (stream != NULL) {
        g_task_set_task_data(task, type, g_free);
        g_task_return_pointer(task, stream, g_object_unref);
    } else {
        g_task_return_error(task, error);
    }
}

static GInputStream *
xfw_wnck_icon_load_finish(GLoadableIcon *icon,
                          GAsyncResult *res,
                          char **type,
                          GError **error) {
    GTask *task;

    g_return_val_if_fail(G_IS_TASK(res), NULL);

    task = G_TASK(res);

    if (!g_task_had_error(task) && type != NULL) {
        *type = g_strdup(g_task_get_task_data(task));
    }

    return g_task_propagate_pointer(task, error);
}

XfwWnckIcon *
_xfw_wnck_icon_new(GObject *wnck_object) {
    g_return_val_if_fail(WNCK_IS_WINDOW(wnck_object) || WNCK_IS_CLASS_GROUP(wnck_object), NULL);

    return g_initable_new(XFW_TYPE_WNCK_ICON,
                          NULL,
                          NULL,
                          "wnck-object", wnck_object,
                          NULL);
}
