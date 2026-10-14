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

#ifndef __XFW_MONITOR_H__
#define __XFW_MONITOR_H__

#if !defined(__LIBXFCE4WINDOWING_H_INSIDE__) && !defined(LIBXFCE4WINDOWING_COMPILATION)
#error "Only libxfce4windowing.h can be included directly"
#endif

#include <gdk/gdk.h>

G_BEGIN_DECLS

/**
 * XfwMonitorTransform:
 * @XFW_MONITOR_TRANSFORM_NORMAL: no transformation applied.
 * @XFW_MONITOR_TRANSFORM_90: rotated counter-clockwise by 90 degrees.
 * @XFW_MONITOR_TRANSFORM_180: rotated counter-clockwise by 180 degrees.
 * @XFW_MONITOR_TRANSFORM_270: rotated counter-clockwise by 270 degrees.
 * @XFW_MONITOR_TRANSFORM_FLIPPED: flipped along a vertical axis.
 * @XFW_MONITOR_TRANSFORM_FLIPPED_90: flipped along a vertical axis and rotated
 * counter-clockwise by 90 degrees.
 * @XFW_MONITOR_TRANSFORM_FLIPPED_180: flipped
 * along a vertical axis and rotated counter-clockwise by 180 degrees.
 * @XFW_MONITOR_TRANSFORM_FLIPPED_270: flipped along a vertical axis and
 * rotated counter-clockwise by 270 degrees.
 *
 * Describes the rotation and reflection applied to a monitor.
 *
 * Since: 4.19.4
 **/
typedef enum _XfwMonitorTransform {
    XFW_MONITOR_TRANSFORM_NORMAL,
    XFW_MONITOR_TRANSFORM_90,
    XFW_MONITOR_TRANSFORM_180,
    XFW_MONITOR_TRANSFORM_270,
    XFW_MONITOR_TRANSFORM_FLIPPED,
    XFW_MONITOR_TRANSFORM_FLIPPED_90,
    XFW_MONITOR_TRANSFORM_FLIPPED_180,
    XFW_MONITOR_TRANSFORM_FLIPPED_270,
} XfwMonitorTransform;

/**
 * XfwMonitorSubpixel:
 * @XFW_MONITOR_SUBPIXEL_UNKNOWN: unknown subpixel ordering.
 * @XFW_MONITOR_SUBPIXEL_NONE: no subpixel geometry.
 * @XFW_MONITOR_SUBPIXEL_HRGB: horizontal RGB.
 * @XFW_MONITOR_SUBPIXEL_HBGR: horizontal BGR.
 * @XFW_MONITOR_SUBPIXEL_VRGB: vertical RGB.
 * @XFW_MONITOR_SUBPIXEL_VBGR: vertical BGR.
 *
 * Describes how the color components of the physical pixels are laid out on a
 * monitor.
 *
 * Since: 4.19.4
 **/
typedef enum _XfwMonitorSubpixel {
    XFW_MONITOR_SUBPIXEL_UNKNOWN,
    XFW_MONITOR_SUBPIXEL_NONE,
    XFW_MONITOR_SUBPIXEL_HRGB,
    XFW_MONITOR_SUBPIXEL_HBGR,
    XFW_MONITOR_SUBPIXEL_VRGB,
    XFW_MONITOR_SUBPIXEL_VBGR,
} XfwMonitorSubpixel;

#define XFW_TYPE_MONITOR_TRANSFORM (xfw_monitor_transform_get_type())
#define XFW_TYPE_MONITOR_SUBPIXEL (xfw_monitor_subpixel_get_type())

#define XFW_TYPE_MONITOR (xfw_monitor_get_type())
G_DECLARE_DERIVABLE_TYPE(XfwMonitor, xfw_monitor, XFW, MONITOR, GObject)

GType xfw_monitor_transform_get_type(void);
GType xfw_monitor_subpixel_get_type(void);

const char *xfw_monitor_get_identifier(XfwMonitor *monitor);
const char *xfw_monitor_get_description(XfwMonitor *monitor);
const char *xfw_monitor_get_connector(XfwMonitor *monitor);
const char *xfw_monitor_get_make(XfwMonitor *monitor);
const char *xfw_monitor_get_model(XfwMonitor *monitor);
const char *xfw_monitor_get_serial(XfwMonitor *monitor);
const unsigned char *xfw_monitor_get_edid(XfwMonitor *monitor, gsize *len);
guint xfw_monitor_get_refresh(XfwMonitor *monitor);
guint xfw_monitor_get_scale(XfwMonitor *monitor);
gdouble xfw_monitor_get_fractional_scale(XfwMonitor *monitor);
void xfw_monitor_get_physical_geometry(XfwMonitor *monitor,
                                       GdkRectangle *physical_geometry);
void xfw_monitor_get_logical_geometry(XfwMonitor *monitor,
                                      GdkRectangle *logical_geometry);
void xfw_monitor_get_workarea(XfwMonitor *monitor,
                              GdkRectangle *workarea);
void xfw_monitor_get_physical_size(XfwMonitor *monitor,
                                   guint *width_mm,
                                   guint *height_mm);
XfwMonitorSubpixel xfw_monitor_get_subpixel(XfwMonitor *monitor);
XfwMonitorTransform xfw_monitor_get_transform(XfwMonitor *monitor);

gboolean xfw_monitor_is_primary(XfwMonitor *monitor);

GdkMonitor *xfw_monitor_get_gdk_monitor(XfwMonitor *monitor);

G_END_DECLS

#endif /* __XFW_MONITOR_H__ */
