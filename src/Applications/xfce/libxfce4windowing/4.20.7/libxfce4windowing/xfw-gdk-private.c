/*
 * Copyright (c) 2024 Brian Tarricone <brian@tarricone.org>
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

#ifdef ENABLE_X11
#include <gdk/gdkx.h>
#endif

#include "xfw-gdk-private.h"

typedef struct {
    GObject parent;

    GdkDisplay *display;
    char *manufacturer;
    char *model;
    char *connector;

    // ... other fields we don't care about...
} XfwGdkMonitorPrivate;

const gchar *
xfw_gdk_monitor_get_connector(GdkMonitor *monitor) {
    g_return_val_if_fail(GDK_IS_MONITOR(monitor), NULL);

    XfwGdkMonitorPrivate *monitor_priv = (XfwGdkMonitorPrivate *)monitor;

    if (monitor_priv->connector == NULL) {
#ifdef ENABLE_X11
        // On X11, the connector name is also stored in the model field.
        if (GDK_IS_X11_MONITOR(monitor)) {
            return gdk_monitor_get_model(monitor);
        }
#endif

        // If we get here, we're on Wayland, and the compositor doesn't
        // support xdg-output, or hasn't sent a xdg_output.name() event.
        // In theory, this *could* still work, but GDK doesn't handle
        // the wl_output.name() event.
        return NULL;
    } else {
        return monitor_priv->connector;
    }
}
