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

#ifndef __XFW_APPLICATION_H__
#define __XFW_APPLICATION_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* fwd decl */
typedef struct _XfwWindow XfwWindow;

#define XFW_TYPE_APPLICATION (xfw_application_get_type())
G_DECLARE_DERIVABLE_TYPE(XfwApplication, xfw_application, XFW, APPLICATION, GObject)

/**
 * XfwApplicationInstance:
 *
 * An opaque structure representing an instance of an #XfwApplication.
 **/
typedef struct _XfwApplicationInstance XfwApplicationInstance;

const gchar *xfw_application_get_class_id(XfwApplication *app);
const gchar *xfw_application_get_name(XfwApplication *app);
GdkPixbuf *xfw_application_get_icon(XfwApplication *app, gint size, gint scale);
GIcon *xfw_application_get_gicon(XfwApplication *app);
gboolean xfw_application_icon_is_fallback(XfwApplication *app);
GList *xfw_application_get_windows(XfwApplication *app);
GList *xfw_application_get_instances(XfwApplication *app);
XfwApplicationInstance *xfw_application_get_instance(XfwApplication *app, XfwWindow *window);

gint xfw_application_instance_get_pid(XfwApplicationInstance *instance);
const gchar *xfw_application_instance_get_name(XfwApplicationInstance *instance);
GList *xfw_application_instance_get_windows(XfwApplicationInstance *instance);

G_END_DECLS

#endif /* !__XFW_APPLICATION_H__ */
