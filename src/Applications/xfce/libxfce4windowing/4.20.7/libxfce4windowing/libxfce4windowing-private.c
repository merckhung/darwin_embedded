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

#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#ifdef ENABLE_X11
#include <gdk/gdkx.h>
#include <libwnck/libwnck.h>
#endif

#include "libxfce4windowing-private.h"
#include "xfw-util.h"

#ifdef ENABLE_X11
#include "xfw-wnck-icon.h"
#endif

static gboolean inited = FALSE;

void
_libxfce4windowing_init(void) {
    if (!inited) {
        inited = TRUE;
        bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
#ifdef HAVE_BIND_TEXTDOMAIN_CODESET
        bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
#endif
    }
}

#ifdef ENABLE_X11
Window
_xfw_wnck_object_get_x11_window(GObject *wnck_object) {
    g_return_val_if_fail(WNCK_IS_WINDOW(wnck_object) || WNCK_IS_CLASS_GROUP(wnck_object), None);

    if (WNCK_IS_WINDOW(wnck_object)) {
        return wnck_window_get_xid(WNCK_WINDOW(wnck_object));
    } else if (WNCK_IS_CLASS_GROUP(wnck_object)) {
        GList *windows = wnck_class_group_get_windows(WNCK_CLASS_GROUP(wnck_object));
        if (windows != NULL) {
            return wnck_window_get_xid(WNCK_WINDOW(windows->data));
        } else {
            return None;
        }
    } else {
        g_warn_if_reached();
        return None;
    }
}

// Icon search priority:
// 1. Primary icon-name
// 2. _NET_WM_ICON/WMHints Pixmap on window
// 3. Secondary icon-name
// 4. Fallback icon-name
GIcon *
_xfw_wnck_object_get_gicon(GObject *wnck_object,
                           const gchar *primary_icon_name,
                           const gchar *secondary_icon_name,
                           const gchar *fallback_icon_name) {
    GIcon *icon;

    g_return_val_if_fail(WNCK_IS_WINDOW(wnck_object) || WNCK_IS_CLASS_GROUP(wnck_object), NULL);
    g_return_val_if_fail(fallback_icon_name != NULL, NULL);

    icon = _xfw_g_icon_new(primary_icon_name);
    if (icon == NULL) {
        icon = G_ICON(_xfw_wnck_icon_new(wnck_object));
        if (G_UNLIKELY(icon == NULL)) {
            icon = _xfw_g_icon_new(secondary_icon_name);
            if (icon == NULL) {
                icon = g_themed_icon_new_with_default_fallbacks(fallback_icon_name);
            }
        }
    }

    return icon;
}
#endif

/**
 * _xfw_g_desktop_app_info_get:
 * @app_id: an application ID
 *
 * Attempts to find a #GDesktopAppInfo instance for the provided application
 * ID.
 *
 * Return value: (nullable) (transfer full): a #GDesktopAppInfo instance,
 * with the reference owned by the caller, or %NULL.
 **/
GDesktopAppInfo *
_xfw_g_desktop_app_info_get(const gchar *app_id) {
    GDesktopAppInfo *app_info;
    gchar *desktop_id;

    desktop_id = g_strdup_printf("%s.desktop", app_id);
    app_info = g_desktop_app_info_new(desktop_id);
    g_free(desktop_id);
    if (app_info == NULL) {
        gchar *lower = g_ascii_strdown(app_id, -1);
        if (g_strcmp0(lower, app_id) != 0) {
            desktop_id = g_strdup_printf("%s.desktop", lower);
            app_info = g_desktop_app_info_new(desktop_id);
            g_free(desktop_id);
        }
        g_free(lower);

        if (app_info == NULL) {
            gchar ***desktop_ids = g_desktop_app_info_search(app_id);
            if (desktop_ids[0] != NULL) {
                app_info = g_desktop_app_info_new(desktop_ids[0][0]);
            }
            for (gchar ***p = desktop_ids; *p != NULL; p++) {
                g_strfreev(*p);
            }
            g_free(desktop_ids);
        }
    }

    return app_info;
}

GdkPixbuf *
_xfw_gicon_load(GIcon *gicon, gint size, gint scale) {
    GtkIconInfo *icon_info;
    GdkPixbuf *icon = NULL;

    icon_info = gtk_icon_theme_lookup_by_gicon_for_scale(gtk_icon_theme_get_default(),
                                                         gicon,
                                                         size,
                                                         scale,
                                                         GTK_ICON_LOOKUP_FORCE_SIZE);
    if (G_LIKELY(icon_info != NULL)) {
        icon = gtk_icon_info_load_icon(icon_info, NULL);
        g_object_unref(icon_info);
    }

    return icon;
}

GIcon *
_xfw_g_icon_new(const gchar *icon_name) {
    if (icon_name != NULL) {
        if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), icon_name)) {
            return g_themed_icon_new(icon_name);
        } else if (g_path_is_absolute(icon_name)
                   && g_file_test(icon_name, G_FILE_TEST_IS_REGULAR))
        {
            GFile *file = g_file_new_for_path(icon_name);
            GIcon *icon = g_file_icon_new(file);
            g_object_unref(file);
            return icon;
        } else {
            GtkIconInfo *icon_info = gtk_icon_theme_lookup_icon(gtk_icon_theme_get_default(),
                                                                icon_name,
                                                                16,
                                                                0);
            if (G_LIKELY(icon_info != NULL)) {
                GIcon *icon = g_themed_icon_new(icon_name);
                g_object_unref(icon_info);
                return icon;
            }
        }
    }

    return NULL;
}
