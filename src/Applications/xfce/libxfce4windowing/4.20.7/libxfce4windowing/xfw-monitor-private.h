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

#ifndef __XFW_MONITOR_PRIVATE_H__
#define __XFW_MONITOR_PRIVATE_H__

#include <gdk/gdk.h>

#include "xfw-monitor.h"

#define MONITORS_CHANGED_MASK ( \
    MONITOR_PENDING_SCALE \
    | MONITOR_PENDING_FRACTIONAL_SCALE \
    | MONITOR_PENDING_PHYSICAL_GEOMETRY \
    | MONITOR_PENDING_LOGICAL_GEOMETRY \
    | MONITOR_PENDING_TRANSFORM \
    | MONITOR_PENDING_IS_PRIMARY)

G_BEGIN_DECLS

struct _XfwMonitorClass {
    GObjectClass parent_class;
};

typedef enum {
    MONITOR_PENDING_IDENTIFIER = (1 << 0),
    MONITOR_PENDING_DESCRIPTION = (1 << 1),
    MONITOR_PENDING_CONNECTOR = (1 << 2),
    MONITOR_PENDING_MAKE = (1 << 3),
    MONITOR_PENDING_MODEL = (1 << 4),
    MONITOR_PENDING_SERIAL = (1 << 5),
    MONITOR_PENDING_REFRESH = (1 << 6),
    MONITOR_PENDING_SCALE = (1 << 7),
    MONITOR_PENDING_PHYSICAL_GEOMETRY = (1 << 8),
    MONITOR_PENDING_LOGICAL_GEOMETRY = (1 << 9),
    MONITOR_PENDING_PHYSICAL_WIDTH = (1 << 10),
    MONITOR_PENDING_PHYSICAL_HEIGHT = (1 << 11),
    MONITOR_PENDING_SUBPIXEL = (1 << 12),
    MONITOR_PENDING_TRANSFORM = (1 << 13),
    MONITOR_PENDING_IS_PRIMARY = (1 << 14),
    MONITOR_PENDING_WORKAREA = (1 << 15),
    MONITOR_PENDING_FRACTIONAL_SCALE = (1 << 16),
    MONITOR_PENDING_EDID = (1 << 17),
} MonitorPendingChanges;

void _xfw_monitor_set_identifier(XfwMonitor *monitor,
                                 const char *identifier);
void _xfw_monitor_set_description(XfwMonitor *monitor,
                                  const char *description);
void _xfw_monitor_set_connector(XfwMonitor *monitor,
                                const char *connector);
void _xfw_monitor_set_make(XfwMonitor *monitor,
                           const char *make);
void _xfw_monitor_set_model(XfwMonitor *monitor,
                            const char *model);
void _xfw_monitor_set_serial(XfwMonitor *monitor,
                             const char *serial);
void _xfw_monitor_set_edid(XfwMonitor *monitor,
                           const unsigned char *edid,
                           gsize edid_len);
void _xfw_monitor_set_refresh(XfwMonitor *monitor,
                              guint refresh_millihertz);
void _xfw_monitor_set_scale(XfwMonitor *monitor,
                            guint scale);
void _xfw_monitor_set_fractional_scale(XfwMonitor *monitor,
                                       gdouble fractional_scale);
void _xfw_monitor_set_physical_geometry(XfwMonitor *monitor,
                                        GdkRectangle *physical_geometry);
void _xfw_monitor_set_logical_geometry(XfwMonitor *monitor,
                                       GdkRectangle *logical_geometry);
void _xfw_monitor_set_workarea(XfwMonitor *monitor,
                               GdkRectangle *workarea);
void _xfw_monitor_set_physical_size(XfwMonitor *monitor,
                                    guint width_mm,
                                    guint height_mm);
void _xfw_monitor_set_subpixel(XfwMonitor *monitor,
                               XfwMonitorSubpixel subpixel);
void _xfw_monitor_set_transform(XfwMonitor *monitor,
                                XfwMonitorTransform transform);

void _xfw_monitor_set_is_primary(XfwMonitor *monitor,
                                 gboolean is_primary);

XfwMonitor *_xfw_monitor_guess_primary_monitor(GList *monitors);
XfwMonitor *_xfw_monitor_from_gdk_monitor(GList *xfwmonitors,
                                          GdkMonitor *gdkmonitor);

gchar *_xfw_monitor_build_identifier(const gchar *make,
                                     const gchar *model,
                                     const gchar *serial,
                                     const gchar *connector);

MonitorPendingChanges _xfw_monitor_notify_pending_changes(XfwMonitor *monitor);

G_END_DECLS

#endif /* __XFW_MONITOR_PRIVATE_H__ */
