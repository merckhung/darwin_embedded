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

#ifndef __XFW_APPLICATION_PRIVATE_H__
#define __XFW_APPLICATION_PRIVATE_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include "xfw-application.h"
#include "xfw-window.h"

#define XFW_APPLICATION_FALLBACK_ICON_NAME "application-x-executable-symbolic"

G_BEGIN_DECLS

struct _XfwApplicationClass {
    /*< private >*/
    GObjectClass parent_class;

    /*< public >*/

    /* Signals */
    void (*icon_changed)(XfwApplication *app);

    /* Virtual Table */
    const gchar *(*get_class_id)(XfwApplication *app);
    const gchar *(*get_name)(XfwApplication *app);
    GIcon *(*get_gicon)(XfwApplication *app);
    GList *(*get_windows)(XfwApplication *app);
    GList *(*get_instances)(XfwApplication *app);
    XfwApplicationInstance *(*get_instance)(XfwApplication *app, XfwWindow *window);
};

struct _XfwApplicationInstance {
    /*< private >*/
    gint pid;
    gchar *name;
    GList *windows;
};

void _xfw_application_invalidate_icon(XfwApplication *app);

G_END_DECLS

#endif /* !__XFW_APPLICATION_PRIVATE_H__ */
