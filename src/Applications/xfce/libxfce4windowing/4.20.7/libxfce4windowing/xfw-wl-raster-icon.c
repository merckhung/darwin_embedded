/*
 * Copyright (c) 2026 Brian Tarricone <brian@tarricone.org>
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
#include <config.h>
#endif

#include <gdk/gdk.h>
#include <gdk/gdkwayland.h>
#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "protocols/xfce-foreign-toplevel-management-private-v1-client.h"

#include "window-icon-utils.h"
#include "xfw-screen-private.h"
#include "xfw-wl-raster-icon.h"

enum {
    PROP_0,
    PROP_WINDOW,
};

struct _XfwWlRasterIcon {
    GObject parent;
    XfwWindowWayland *window;

    WindowIcon *window_icon;
    guint window_icon_size;
    guint window_icon_scale;
    enum xfce_foreign_toplevel_icon_pixels_v1_failure_reason failure_reason;
};

struct _XfwWlRasterIconClass {
    GObjectClass parent_class;
};

static void xfw_wl_raster_icon_set_property(GObject *object,
                                            guint prop_id,
                                            const GValue *value,
                                            GParamSpec *pspec);
static void xfw_wl_raster_icon_get_property(GObject *object,
                                            guint prop_id,
                                            GValue *value,
                                            GParamSpec *pspec);
static void xfw_wl_raster_icon_dispose(GObject *object);
static void xfw_wl_raster_icon_finalize(GObject *object);

static void xfw_wl_raster_icon_gicon_init(GIconIface *iface);
static void xfw_wl_raster_icon_loadable_icon_init(GLoadableIconIface *iface);

static gboolean xfw_wl_raster_icon_equal(GIcon *icon1,
                                         GIcon *icon2);
static guint xfw_wl_raster_icon_hash(GIcon *icon);

static GInputStream *xfw_wl_raster_icon_load(GLoadableIcon *icon,
                                             int size,
                                             char **type,
                                             GCancellable *cancellable,
                                             GError **error);
static void xfw_wl_raster_icon_load_async(GLoadableIcon *icon,
                                          int size,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
static GInputStream *xfw_wl_raster_icon_load_finish(GLoadableIcon *icon,
                                                    GAsyncResult *res,
                                                    char **type,
                                                    GError **error);

static void pixels_received(void *data,
                            struct xfce_foreign_toplevel_icon_pixels_v1 *pixels,
                            int32_t fd,
                            uint32_t width,
                            uint32_t height,
                            uint32_t stride);
static void pixels_failed(void *data,
                          struct xfce_foreign_toplevel_icon_pixels_v1 *pixels,
                          enum xfce_foreign_toplevel_icon_pixels_v1_failure_reason reason);

static GInputStream *create_input_stream(WindowIcon *icon);

static const struct xfce_foreign_toplevel_icon_pixels_v1_listener pixels_listener = {
    .pixels = pixels_received,
    .failed = pixels_failed,
};

G_DEFINE_FINAL_TYPE_WITH_CODE(XfwWlRasterIcon,
                              xfw_wl_raster_icon,
                              G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_ICON, xfw_wl_raster_icon_gicon_init)
                              G_IMPLEMENT_INTERFACE(G_TYPE_LOADABLE_ICON, xfw_wl_raster_icon_loadable_icon_init))

static void
xfw_wl_raster_icon_class_init(XfwWlRasterIconClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->set_property = xfw_wl_raster_icon_set_property;
    gobject_class->get_property = xfw_wl_raster_icon_get_property;
    gobject_class->dispose = xfw_wl_raster_icon_dispose;
    gobject_class->finalize = xfw_wl_raster_icon_finalize;

    g_object_class_install_property(gobject_class,
                                    PROP_WINDOW,
                                    g_param_spec_object("window",
                                                        "window",
                                                        "XfwWindowWayland",
                                                        XFW_TYPE_WINDOW_WAYLAND,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_wl_raster_icon_set_property(GObject *object,
                                guint prop_id,
                                const GValue *value,
                                GParamSpec *pspec) {
    XfwWlRasterIcon *icon = XFW_WL_RASTER_ICON(object);

    switch (prop_id) {
        case PROP_WINDOW:
            icon->window = g_value_dup_object(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
xfw_wl_raster_icon_get_property(GObject *object,
                                guint prop_id,
                                GValue *value,
                                GParamSpec *pspec) {
    XfwWlRasterIcon *icon = XFW_WL_RASTER_ICON(object);

    switch (prop_id) {
        case PROP_WINDOW:
            g_value_set_object(value, icon->window);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
xfw_wl_raster_icon_dispose(GObject *object) {
    XfwWlRasterIcon *icon = XFW_WL_RASTER_ICON(object);

    g_clear_object(&icon->window);

    G_OBJECT_CLASS(xfw_wl_raster_icon_parent_class)->dispose(object);
}

static void
xfw_wl_raster_icon_finalize(GObject *object) {
    XfwWlRasterIcon *icon = XFW_WL_RASTER_ICON(object);

    g_clear_pointer(&icon->window_icon, _window_icon_free);

    G_OBJECT_CLASS(xfw_wl_raster_icon_parent_class)->finalize(object);
}

static void
xfw_wl_raster_icon_gicon_init(GIconIface *iface) {
    iface->equal = xfw_wl_raster_icon_equal;
    iface->hash = xfw_wl_raster_icon_hash;
}

static void
xfw_wl_raster_icon_loadable_icon_init(GLoadableIconIface *iface) {
    iface->load = xfw_wl_raster_icon_load;
    iface->load_async = xfw_wl_raster_icon_load_async;
    iface->load_finish = xfw_wl_raster_icon_load_finish;
}

static void
xfw_wl_raster_icon_init(XfwWlRasterIcon *icon) {}

static gboolean
xfw_wl_raster_icon_equal(GIcon *icon1, GIcon *icon2) {
    if (!XFW_IS_WL_RASTER_ICON(icon1) || !XFW_IS_WL_RASTER_ICON(icon2)) {
        return FALSE;
    } else {
        return XFW_WL_RASTER_ICON(icon1)->window == XFW_WL_RASTER_ICON(icon2)->window;
    }
}

static guint
xfw_wl_raster_icon_hash(GIcon *icon) {
    XfwWlRasterIcon *raster_icon = XFW_WL_RASTER_ICON(icon);
    return g_direct_hash(raster_icon->window);
}

static GInputStream *
xfw_wl_raster_icon_load(GLoadableIcon *icon,
                        int size,
                        char **type,
                        GCancellable *cancellable,
                        GError **error) {
    XfwWlRasterIcon *raster_icon = XFW_WL_RASTER_ICON(icon);

    guint desired_scale = 1;
    for (GList *lm = xfw_window_get_monitors(XFW_WINDOW(raster_icon->window)); lm != NULL; lm = lm->next) {
        desired_scale = MAX(desired_scale, xfw_monitor_get_scale(XFW_MONITOR(lm->data)));
    }
    guint desired_size = size / desired_scale;

    GList *icon_sizes = _xfw_window_wayland_get_icon_sizes(raster_icon->window);
    if (icon_sizes == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("This window does not have any raster icons"));
        return NULL;
    } else if (raster_icon->window_icon != NULL && raster_icon->window_icon_scale == desired_scale && raster_icon->window_icon_size == desired_size) {
        GInputStream *is = create_input_stream(raster_icon->window_icon);
        if (is != NULL) {
            return is;
        } else {
            g_clear_pointer(&raster_icon->window_icon, _window_icon_free);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Unknown error loading icon"));
            return NULL;
        }
    } else {
        IconSize *best_size = NULL;
        for (GList *ls = icon_sizes; ls != NULL; ls = ls->next) {
            IconSize *icon_size = ls->data;
            if (icon_size->scale == desired_scale && icon_size->size >= desired_size) {
                best_size = icon_size;
                break;
            }
        }

        if (best_size == NULL) {
            for (GList *ls = icon_sizes; ls != NULL; ls = ls->next) {
                IconSize *icon_size = ls->data;
                if (icon_size->size * icon_size->scale >= (guint)size) {
                    best_size = icon_size;
                    break;
                }
            }
        }

        if (best_size == NULL) {
            best_size = g_list_last(icon_sizes)->data;
        }

        // Shouldn't be possible, but...
        g_return_val_if_fail(best_size != NULL, NULL);

        struct xfce_foreign_toplevel_handle_v1 *xfce_handle = _xfw_window_wayland_get_xfce_handle(raster_icon->window);
        struct xfce_foreign_toplevel_icon_pixels_v1 *pixels = xfce_foreign_toplevel_handle_v1_get_icon_pixels(xfce_handle, best_size->size, best_size->scale);
        xfce_foreign_toplevel_icon_pixels_v1_add_listener(pixels, &pixels_listener, raster_icon);

        XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(raster_icon->window));
        GdkScreen *gscreen = _xfw_screen_get_gdk_screen(screen);
        GdkDisplay *display = gdk_screen_get_display(gscreen);
        struct wl_display *wl_display = gdk_wayland_display_get_wl_display(display);
        wl_display_roundtrip(wl_display);

        xfce_foreign_toplevel_icon_pixels_v1_destroy(pixels);

        if (raster_icon->window_icon == NULL) {
            switch (raster_icon->failure_reason) {
                case XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_NO_PIXEL_DATA:
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("This window does not have any raster icons"));
                    return NULL;

                case XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_INVALID_ARGS:
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("The requested size/scale pair does not exist"));
                    return NULL;

                default:
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Unknown error loading icon"));
                    return NULL;
            }
        } else {
            GInputStream *is = create_input_stream(raster_icon->window_icon);
            if (is != NULL) {
                raster_icon->window_icon_scale = best_size->scale;
                raster_icon->window_icon_size = best_size->size;
                return is;
            } else {
                g_clear_pointer(&raster_icon->window_icon, _window_icon_free);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Unknown error loading icon"));
                return NULL;
            }
        }
    }
}

static void
xfw_wl_raster_icon_load_async(GLoadableIcon *icon,
                              int size,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data) {
    GInputStream *stream;
    GTask *task = g_task_new(icon, cancellable, callback, user_data);
    gchar *type = NULL;
    GError *error = NULL;

    stream = xfw_wl_raster_icon_load(icon, size, &type, cancellable, &error);
    if (stream != NULL) {
        g_task_set_task_data(task, type, g_free);
        g_task_return_pointer(task, stream, g_object_unref);
    } else {
        g_task_return_error(task, error);
    }
}

static GInputStream *
xfw_wl_raster_icon_load_finish(GLoadableIcon *icon,
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

static void
pixels_received(void *data,
                struct xfce_foreign_toplevel_icon_pixels_v1 *pixels,
                int32_t fd,
                uint32_t width,
                uint32_t height,
                uint32_t stride) {
    XfwWlRasterIcon *raster_icon = XFW_WL_RASTER_ICON(data);

    size_t len = stride * height;
    size_t packed_len = width * 4 * height;

    if (len < packed_len) {
        raster_icon->failure_reason = XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_UNKNOWN;
    } else {
        void *mapping = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) {
            raster_icon->failure_reason = XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_UNKNOWN;
        } else {
            guint32 *argb32;
            if (len == packed_len) {
                argb32 = (guint32 *)mapping;
            } else {
                argb32 = g_malloc(packed_len);
                for (gsize j = 0; j < height; ++j) {
                    memcpy(((guchar *)argb32) + j * width * 4, ((guchar *)mapping) + j * stride, width * 4);
                }
            }

            raster_icon->window_icon = _window_icon_new(argb32, width, height, TRUE);
            if (raster_icon->window_icon == NULL) {
                raster_icon->failure_reason = XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_UNKNOWN;
            }

            munmap(mapping, len);
            if (len != packed_len) {
                g_free(argb32);
            }
        }
    }

    close(fd);
}

static void
pixels_failed(void *data,
              struct xfce_foreign_toplevel_icon_pixels_v1 *pixels,
              enum xfce_foreign_toplevel_icon_pixels_v1_failure_reason reason) {
    XfwWlRasterIcon *raster_icon = XFW_WL_RASTER_ICON(data);
    raster_icon->failure_reason = reason;
}

static GInputStream *
create_input_stream(WindowIcon *window_icon) {
    guchar *bmp = g_memdup2(window_icon->bmp, window_icon->bmp_len);
    return g_memory_input_stream_new_from_data(bmp, window_icon->bmp_len, g_free);
}

XfwWlRasterIcon *
_xfw_wl_raster_icon_new(XfwWindowWayland *window) {
    g_return_val_if_fail(XFW_IS_WINDOW_WAYLAND(window), NULL);
    return g_object_new(XFW_TYPE_WL_RASTER_ICON, "window", window, NULL);
}
