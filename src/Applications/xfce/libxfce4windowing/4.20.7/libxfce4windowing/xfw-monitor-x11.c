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

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/randr.h>
#include <X11/extensions/render.h>
#include <gdk/gdkx.h>
#include <glib/gi18n-lib.h>
#include <libdisplay-info/info.h>
#include <stdlib.h>

#include "xfw-monitor-private.h"
#include "xfw-monitor-x11.h"
#include "xfw-monitor.h"
#include "xfw-screen-x11.h"
#include "xfw-screen.h"
#include "xsettings-x11.h"

struct _XfwMonitorManagerX11 {
    XfwScreenX11 *screen;
    int xrandr_event_base;
    XSettingsX11 *xsettings;
    gint scale;
    guint refresh_idle_id;
};

struct _XfwMonitorX11 {
    XfwMonitor parent;
};


G_DEFINE_FINAL_TYPE(XfwMonitorX11, xfw_monitor_x11, XFW_TYPE_MONITOR)


static void
xfw_monitor_x11_class_init(XfwMonitorX11Class *klass) {}

static void
xfw_monitor_x11_init(XfwMonitorX11 *monitor) {}

static int
xrandr_init(Display *dpy, const gchar **error) {
    int evbase, errbase;
    if (!XRRQueryExtension(dpy, &evbase, &errbase)) {
        *error = "extension not found";
        return -1;
    }

    int major, minor;
    if (!XRRQueryVersion(dpy, &major, &minor)) {
        *error = "version query failed";
        return -1;
    }
    if (major != 1 || minor < 5) {
        *error = "version 1.5 or better required";
        return -1;
    }

    return evbase;
}

static inline XfwMonitorSubpixel
xfw_subpixel_from_x11(SubpixelOrder x11_subpixel) {
    switch (x11_subpixel) {
        case SubPixelNone:
            return XFW_MONITOR_SUBPIXEL_NONE;
        case SubPixelHorizontalRGB:
            return XFW_MONITOR_SUBPIXEL_HRGB;
        case SubPixelHorizontalBGR:
            return XFW_MONITOR_SUBPIXEL_HBGR;
        case SubPixelVerticalRGB:
            return XFW_MONITOR_SUBPIXEL_VRGB;
        case SubPixelVerticalBGR:
            return XFW_MONITOR_SUBPIXEL_VBGR;
        default:
            return XFW_MONITOR_SUBPIXEL_UNKNOWN;
    }
}

static inline XfwMonitorTransform
xfw_transform_from_x11(Rotation x11_rotation) {
    switch (x11_rotation) {
        case RR_Rotate_90:
            return XFW_MONITOR_TRANSFORM_90;
        case RR_Rotate_180:
            return XFW_MONITOR_TRANSFORM_180;
        case RR_Rotate_270:
            return XFW_MONITOR_TRANSFORM_270;
        case (RR_Rotate_0 | RR_Reflect_X):
            return XFW_MONITOR_TRANSFORM_FLIPPED;
        case (RR_Rotate_90 | RR_Reflect_X):
            return XFW_MONITOR_TRANSFORM_FLIPPED_90;
        case (RR_Rotate_180 | RR_Reflect_X):
            return XFW_MONITOR_TRANSFORM_FLIPPED_180;
        case (RR_Rotate_270 | RR_Reflect_X):
            return XFW_MONITOR_TRANSFORM_FLIPPED_270;
        case (RR_Rotate_0 | RR_Reflect_Y):
            return XFW_MONITOR_TRANSFORM_FLIPPED_180;
        case (RR_Rotate_90 | RR_Reflect_Y):
            return XFW_MONITOR_TRANSFORM_FLIPPED_270;
        case (RR_Rotate_180 | RR_Reflect_Y):
            return XFW_MONITOR_TRANSFORM_FLIPPED;
        case (RR_Rotate_270 | RR_Reflect_Y):
            return XFW_MONITOR_TRANSFORM_FLIPPED_90;
        default:
            return XFW_MONITOR_TRANSFORM_NORMAL;
    }
}

static gboolean
get_cardinal_prop(GdkDisplay *display, Window xroot, const char *prop_name, gint *value) {
    Display *dpy = gdk_x11_display_get_xdisplay(display);
    Atom actual_type;
    int actual_format;
    gulong nitems = 0;
    gulong bytes_after = 0;
    guchar *prop = NULL;

    gdk_x11_display_error_trap_push(display);
    int ret = XGetWindowProperty(dpy,
                                 xroot,
                                 XInternAtom(dpy, prop_name, False),
                                 0,
                                 sizeof(gint),
                                 False,
                                 XA_CARDINAL,
                                 &actual_type,
                                 &actual_format,
                                 &nitems,
                                 &bytes_after,
                                 &prop);
    if (gdk_x11_display_error_trap_pop(display) != 0
        || ret != Success
        || actual_type != XA_CARDINAL
        || actual_format != 32
        || nitems != 1
        || bytes_after != 0)
    {
        g_clear_pointer(&prop, XFree);
        return FALSE;
    } else {
        *value = ((gulong *)(gpointer)prop)[0];
        XFree(prop);
        return TRUE;
    }
}

static void
update_monitor_workarea(XfwScreenX11 *screen, XfwMonitor *monitor, gint cur_workspace_num) {
    GArray *workareas = _xfw_screen_x11_get_workareas(screen);
    if (workareas != NULL && cur_workspace_num <= (gint)workareas->len - 1) {
        GdkRectangle geom;
        xfw_monitor_get_logical_geometry(monitor, &geom);

        GdkRectangle *workarea = &g_array_index(workareas, GdkRectangle, cur_workspace_num);
        if (gdk_rectangle_intersect(&geom, workarea, &geom)) {
            _xfw_monitor_set_workarea(monitor, &geom);
        }
    } else {
        g_debug("We're on workspace %d, but only have %u workareas",
                cur_workspace_num,
                workareas != NULL ? workareas->len : 0);
    }
}

static void
update_monitor_workareas_for_workspace(XfwScreenX11 *screen, gint cur_workspace_num) {
    for (GList *l = xfw_screen_get_monitors(XFW_SCREEN(screen)); l != NULL; l = l->next) {
        XfwMonitor *monitor = XFW_MONITOR(l->data);
        update_monitor_workarea(screen, monitor, cur_workspace_num);
    }
    for (GList *l = xfw_screen_get_monitors(XFW_SCREEN(screen)); l != NULL; l = l->next) {
        XfwMonitor *monitor = XFW_MONITOR(l->data);
        _xfw_monitor_notify_pending_changes(monitor);
    }
}

static void
update_monitor_workareas(XfwMonitorManagerX11 *manager) {
    GdkScreen *gscreen = _xfw_screen_get_gdk_screen(XFW_SCREEN(manager->screen));
    GdkDisplay *display = gdk_screen_get_display(gscreen);
    Window root = gdk_x11_window_get_xid(gdk_screen_get_root_window(gscreen));

    gint cur_workspace_num = 0;
    if (!get_cardinal_prop(display, root, "_NET_CURRENT_DESKTOP", &cur_workspace_num)) {
        g_message("Failed to fetch _NET_CURRENT_DESKTOP; assuming 0");
    }

    update_monitor_workareas_for_workspace(manager->screen, cur_workspace_num);
}

static void
update_workareas(XfwMonitorManagerX11 *manager) {
    GdkScreen *gdkscreen = _xfw_screen_get_gdk_screen(XFW_SCREEN(manager->screen));
    GdkWindow *root = gdk_screen_get_root_window(gdkscreen);
    Window xroot = gdk_x11_window_get_xid(root);

    GdkDisplay *display = gdk_screen_get_display(gdkscreen);
    Display *dpy = gdk_x11_display_get_xdisplay(display);

    gint workspace_count = 1;
    if (!get_cardinal_prop(display, xroot, "_NET_NUMBER_OF_DESKTOPS", &workspace_count)) {
        g_message("Failed to fetch _NET_NUMBER_OF_DESKTOPS; assuming 1");
    }

    Atom actual_type;
    int actual_format;
    gulong nitems = 0;
    gulong bytes_after = 0;
    guchar *prop = NULL;

    GArray *workareas;

    gdk_x11_display_error_trap_push(display);
    int ret = XGetWindowProperty(dpy,
                                 xroot,
                                 XInternAtom(dpy, "_NET_WORKAREA", False),
                                 0,
                                 sizeof(unsigned int) * 4 * workspace_count,
                                 False,
                                 XA_CARDINAL,
                                 &actual_type,
                                 &actual_format,
                                 &nitems,
                                 &bytes_after,
                                 &prop);
    if (gdk_x11_display_error_trap_pop(display) != 0
        || ret != Success
        || actual_type != XA_CARDINAL
        || actual_format != 32
        || nitems < 4
        || nitems % 4 != 0)
    {
        g_message("Failed to get _NET_WORKAREA; using full screen dimensions");
        Screen *xscreen = gdk_x11_screen_get_xscreen(gdkscreen);
        GdkRectangle screen_geom = {
            .x = 0,
            .y = 0,
            .width = WidthOfScreen(xscreen),
            .height = HeightOfScreen(xscreen),
        };
        workareas = g_array_sized_new(FALSE, TRUE, sizeof(GdkRectangle), workspace_count);
        for (gint i = 0; i < workspace_count; ++i) {
            g_array_append_val(workareas, screen_geom);
        }
    } else {
        gint nworkareas = nitems / 4;
        long *workareas_raw = (long *)(gpointer)prop;

        if (nworkareas < workspace_count) {
            g_message("We got %d as the workspace count, but there are only %d workareas returned",
                      workspace_count,
                      nworkareas);
        }

        workareas = g_array_sized_new(FALSE, TRUE, sizeof(GdkRectangle), nworkareas);
        for (gint i = 0; i < nworkareas; ++i) {
            GdkRectangle workarea = {
                .x = workareas_raw[i * 4] / manager->scale,
                .y = workareas_raw[i * 4 + 1] / manager->scale,
                .width = workareas_raw[i * 4 + 2] / manager->scale,
                .height = workareas_raw[i * 4 + 3] / manager->scale,
            };
            g_array_append_val(workareas, workarea);
        }
    }
    g_clear_pointer(&prop, XFree);

    _xfw_screen_x11_set_workareas(manager->screen, workareas);
}

static void
ensure_workareas(XfwMonitorManagerX11 *manager) {
    if (_xfw_screen_x11_get_workareas(manager->screen) == NULL) {
        update_workareas(manager);
        update_monitor_workareas(manager);
    }
}

static XfwMonitor *
steal_monitor_by_connector(GList **monitors, const char *connector) {
    for (GList *l = *monitors; l != NULL; l = l->next) {
        XfwMonitor *monitor = XFW_MONITOR(l->data);
        if (g_strcmp0(connector, xfw_monitor_get_connector(monitor)) == 0) {
            *monitors = g_list_delete_link(*monitors, l);
            return monitor;
        }
    }
    return NULL;
}

static XfwMonitor *
create_fallback_monitor(Display *dpy, XfwScreenX11 *screen, gint cur_workspace_num) {
    XfwMonitor *monitor = g_object_new(XFW_TYPE_MONITOR_X11, NULL);

    _xfw_monitor_set_connector(monitor, "default");
    _xfw_monitor_set_scale(monitor, 1);
    _xfw_monitor_set_fractional_scale(monitor, 1.0);
    _xfw_monitor_set_physical_size(monitor, DisplayWidthMM(dpy, 0), DisplayHeightMM(dpy, 0));

    GdkRectangle geometry = {
        .x = 0,
        .y = 0,
        .width = DisplayWidth(dpy, 0),
        .height = DisplayHeight(dpy, 0),
    };
    _xfw_monitor_set_physical_geometry(monitor, &geometry);
    _xfw_monitor_set_logical_geometry(monitor, &geometry);

    update_monitor_workarea(screen, monitor, cur_workspace_num);

    _xfw_monitor_set_make(monitor, _("Unknown"));
    _xfw_monitor_set_model(monitor, _("Unknown"));

    const char *make = xfw_monitor_get_make(monitor);
    const char *model = xfw_monitor_get_model(monitor);
    const char *serial = xfw_monitor_get_serial(monitor);
    const char *connector = xfw_monitor_get_connector(monitor);

    gchar *identifier = _xfw_monitor_build_identifier(make, model, serial, connector);
    _xfw_monitor_set_identifier(monitor, identifier);
    g_free(identifier);

    _xfw_monitor_set_description(monitor, _("Default Monitor"));

    return monitor;
}

static GList *
enumerate_monitors(XfwMonitorManagerX11 *manager, GList **new_monitors, GList **previous_monitors) {
    GdkScreen *gscreen = _xfw_screen_get_gdk_screen(XFW_SCREEN(manager->screen));
    GdkDisplay *display = gdk_screen_get_display(gscreen);
    Display *dpy = gdk_x11_display_get_xdisplay(display);
    Window root = XDefaultRootWindow(dpy);

    XRRScreenResources *resources = XRRGetScreenResourcesCurrent(dpy, root);
    if (resources == NULL) {
        g_message("XRRGetScreenResourcesCurrent() failed");
        return NULL;
    }

    ensure_workareas(manager);
    gint cur_workspace_num = 0;
    if (!get_cardinal_prop(display, root, "_NET_CURRENT_DESKTOP", &cur_workspace_num)) {
        g_message("Failed to fetch _NET_CURRENT_DESKTOP; assuming 0");
    }

    int nmonitors = 0;
    XRRMonitorInfo *rrmonitors = XRRGetMonitors(dpy, root, True, &nmonitors);

    GList *monitors = NULL;
    XfwMonitor *primary_monitor = NULL;

    for (int i = 0; i < nmonitors; ++i) {
        RROutput output = rrmonitors[i].outputs[0];

        gdk_x11_display_error_trap_push(display);
        XRROutputInfo *oinfo = XRRGetOutputInfo(dpy, resources, output);
        if (gdk_x11_display_error_trap_pop(display) != 0
            || oinfo == NULL
            || oinfo->connection == RR_Disconnected
            || oinfo->crtc == None)
        {
            if (oinfo != NULL) {
                XRRFreeOutputInfo(oinfo);
            }
            continue;
        }

        gdk_x11_display_error_trap_push(display);
        XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, resources, oinfo->crtc);
        if (gdk_x11_display_error_trap_pop(display) != 0 || crtc == NULL) {
            XRRFreeOutputInfo(oinfo);
            continue;
        }

        gchar *connector = g_strdup(gdk_x11_get_xatom_name_for_display(display, rrmonitors[i].name));
        XfwMonitor *monitor = steal_monitor_by_connector(previous_monitors, connector);
        if (monitor == NULL) {
            monitor = g_object_new(XFW_TYPE_MONITOR_X11, NULL);
            *new_monitors = g_list_append(*new_monitors, monitor);
            _xfw_monitor_set_connector(monitor, connector);
        }

        _xfw_monitor_set_scale(monitor, manager->scale);
        _xfw_monitor_set_fractional_scale(monitor, manager->scale);
        _xfw_monitor_set_physical_size(monitor, oinfo->mm_width, oinfo->mm_height);
        _xfw_monitor_set_subpixel(monitor, xfw_subpixel_from_x11(oinfo->subpixel_order));
        _xfw_monitor_set_transform(monitor, xfw_transform_from_x11(crtc->rotation));

        for (gint j = 0; j < resources->nmode; ++j) {
            XRRModeInfo *mode = &resources->modes[j];
            if (mode->id == crtc->mode) {
                if (mode->hTotal > 0 && mode->vTotal > 0) {
                    _xfw_monitor_set_refresh(monitor, (mode->dotClock * 1000) / (mode->hTotal * mode->vTotal));
                    break;
                }
            }
        }

        GdkRectangle geometry = {
            .x = rrmonitors[i].x,
            .y = rrmonitors[i].y,
            .width = rrmonitors[i].width,
            .height = rrmonitors[i].height,
        };
        _xfw_monitor_set_physical_geometry(monitor, &geometry);

        geometry.x /= manager->scale;
        geometry.y /= manager->scale;
        geometry.width /= manager->scale;
        geometry.height /= manager->scale;
        _xfw_monitor_set_logical_geometry(monitor, &geometry);

        update_monitor_workarea(manager->screen, monitor, cur_workspace_num);

        XRRFreeCrtcInfo(crtc);

        Atom edid_atom = XInternAtom(dpy, RR_PROPERTY_RANDR_EDID, False);

        gdk_x11_display_error_trap_push(display);

        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nbytes = 0;
        unsigned long bytes_left = 0;
        unsigned char *edid_data = NULL;

        XRRGetOutputProperty(dpy,
                             output,
                             edid_atom,
                             0,
                             256,
                             False,
                             False,
                             AnyPropertyType,
                             &actual_type,
                             &actual_format,
                             &nbytes,
                             &bytes_left,
                             &edid_data);

        if (gdk_x11_display_error_trap_pop(display) == 0 && edid_data != NULL && nbytes > 0) {
            struct di_info *edid_info = di_info_parse_edid(edid_data, nbytes);
            if (edid_info != NULL) {
                char *make = di_info_get_make(edid_info);
                if (make != NULL) {
                    _xfw_monitor_set_make(monitor, make);
                }
                free(make);

                char *model = di_info_get_model(edid_info);
                if (model != NULL) {
                    _xfw_monitor_set_model(monitor, model);
                }
                free(model);

                char *serial = di_info_get_serial(edid_info);
                if (serial != NULL) {
                    _xfw_monitor_set_serial(monitor, serial);
                }
                free(serial);

                di_info_destroy(edid_info);
            }

            _xfw_monitor_set_edid(monitor, edid_data, nbytes);
        }
        if (edid_data != NULL) {
            XFree(edid_data);
        }

        const char *make = xfw_monitor_get_make(monitor);
        const char *model = xfw_monitor_get_model(monitor);
        const char *serial = xfw_monitor_get_serial(monitor);

        gchar *identifier = _xfw_monitor_build_identifier(make, model, serial, connector);
        _xfw_monitor_set_identifier(monitor, identifier);
        g_free(identifier);

        char *description;
        if (make != NULL && model != NULL && serial != NULL) {
            description = g_strdup_printf("%s %s %s (%s)", make, model, serial, connector);
        } else if (make != NULL && model != NULL) {
            description = g_strdup_printf("%s %s (%s)", make, model, connector);
        } else if (make != NULL) {
            description = g_strdup_printf("%s (%s)", make, connector);
        } else {
            description = g_strdup(connector);
        }
        _xfw_monitor_set_description(monitor, description);
        g_free(description);

        _xfw_monitor_set_is_primary(monitor, !!rrmonitors[i].primary);
        if (rrmonitors[i].primary) {
            primary_monitor = monitor;
        }

        monitors = g_list_prepend(monitors, monitor);

        g_free(connector);
        XRRFreeOutputInfo(oinfo);
    }
    monitors = g_list_reverse(monitors);

    XRRFreeScreenResources(resources);
    if (rrmonitors != NULL) {
        XRRFreeMonitors(rrmonitors);
    }

    if (monitors == NULL) {
        monitors = g_list_append(monitors, create_fallback_monitor(dpy, manager->screen, cur_workspace_num));
    }

    if (primary_monitor == NULL) {
        primary_monitor = _xfw_monitor_guess_primary_monitor(monitors);
        if (primary_monitor != NULL) {
            _xfw_monitor_set_is_primary(primary_monitor, TRUE);
        }
    }

    return monitors;
}

static void
refresh_monitors(XfwMonitorManagerX11 *manager) {
    GList *new_monitors = NULL;
    GList *previous_monitors = _xfw_screen_steal_monitors(XFW_SCREEN(manager->screen));
    GList *monitors = enumerate_monitors(manager, &new_monitors, &previous_monitors);

    _xfw_screen_set_monitors(XFW_SCREEN(manager->screen), monitors, new_monitors, previous_monitors);

    g_list_free(new_monitors);
    g_list_free_full(previous_monitors, g_object_unref);
}

static gboolean
refresh_monitors_idled(gpointer data) {
    XfwMonitorManagerX11 *manager = data;
    manager->refresh_idle_id = 0;
    refresh_monitors(manager);
    return FALSE;
}

static GdkFilterReturn
rootwin_event_filter(GdkXEvent *gxevent, GdkEvent *event, gpointer data) {
    XfwMonitorManagerX11 *manager = data;
    XEvent *xevent = (XEvent *)gxevent;

    if (manager->xrandr_event_base != -1
        && (xevent->type - manager->xrandr_event_base == RRScreenChangeNotify
            || xevent->type - manager->xrandr_event_base == RRNotify))
    {
        if (manager->refresh_idle_id != 0) {
            g_source_remove(manager->refresh_idle_id);
        }
        manager->refresh_idle_id = g_idle_add(refresh_monitors_idled, manager);
    } else if (xevent->type == PropertyNotify
               && xevent->xproperty.atom == XInternAtom(xevent->xproperty.display, "_NET_WORKAREA", False))
    {
        update_workareas(manager);
        update_monitor_workareas(manager);
    }

    return GDK_FILTER_CONTINUE;
}

static void
scale_factor_changed(gint scale, gpointer data) {
    XfwMonitorManagerX11 *manager = data;
    if (scale != manager->scale) {
        manager->scale = scale;
        update_workareas(manager);

        if (manager->xrandr_event_base != -1) {
            if (manager->refresh_idle_id != 0) {
                g_source_remove(manager->refresh_idle_id);
            }
            manager->refresh_idle_id = g_idle_add(refresh_monitors_idled, manager);
        } else {
            GList *monitors = _xfw_screen_steal_monitors(XFW_SCREEN(manager->screen));

            for (GList *l = monitors; l != NULL; l = l->next) {
                XfwMonitor *monitor = XFW_MONITOR(l->data);
                _xfw_monitor_set_scale(monitor, manager->scale);
                _xfw_monitor_set_fractional_scale(monitor, manager->scale);

                GdkRectangle geom;
                xfw_monitor_get_physical_geometry(monitor, &geom);
                geom.x /= manager->scale;
                geom.y /= manager->scale;
                geom.width /= manager->scale;
                geom.height /= manager->scale;
                _xfw_monitor_set_logical_geometry(monitor, &geom);
            }

            _xfw_screen_set_monitors(XFW_SCREEN(manager->screen), monitors, NULL, NULL);
        }
    }
}

static gboolean
parse_gdk_scale_env(gint *scale) {
    const gchar *gdk_scale_str = g_getenv("GDK_SCALE");
    char *endptr = NULL;
    errno = 0;
    gint gdk_scale = gdk_scale_str != NULL ? strtol(gdk_scale_str, &endptr, 10) : 0;
    if (gdk_scale > 0 && endptr != NULL && *endptr == '\0' && errno == 0) {
        *scale = gdk_scale;
        return TRUE;
    } else {
        return FALSE;
    }
}

XfwMonitorManagerX11 *
_xfw_monitor_manager_x11_new(XfwScreenX11 *xscreen) {
    XfwMonitorManagerX11 *manager = g_new0(XfwMonitorManagerX11, 1);
    manager->screen = xscreen;
    manager->scale = 1;

    XfwScreen *screen = XFW_SCREEN(xscreen);
    GdkScreen *gscreen = _xfw_screen_get_gdk_screen(screen);

    if (!parse_gdk_scale_env(&manager->scale)) {
        manager->xsettings = _xsettings_x11_new(gscreen, scale_factor_changed, manager);
        manager->scale = _xsettings_x11_get_scale(manager->xsettings);
    }

    GdkDisplay *display = gdk_screen_get_display(gscreen);
    Display *dpy = gdk_x11_display_get_xdisplay(gdk_screen_get_display(gscreen));
    GdkWindow *rootwin = gdk_screen_get_root_window(gscreen);
    Window xrootwin = gdk_x11_window_get_xid(rootwin);

    const gchar *error = NULL;
    manager->xrandr_event_base = xrandr_init(dpy, &error);
    if (manager->xrandr_event_base == -1) {
        g_message("XRandR initialization error: %s", error);
        g_message("Will advertise only a single monitor");

        const gchar *connector = "X11-1";

        XfwMonitor *monitor = g_object_new(XFW_TYPE_MONITOR_X11, NULL);
        _xfw_monitor_set_connector(monitor, connector);
        _xfw_monitor_set_description(monitor, "X11 Monitor (X11-1)");
        _xfw_monitor_set_refresh(monitor, 60000);

        Screen *x11screen = gdk_x11_screen_get_xscreen(gscreen);
        GdkRectangle geom = {
            .x = 0,
            .y = 0,
            .width = WidthOfScreen(x11screen),
            .height = HeightOfScreen(x11screen),
        };
        _xfw_monitor_set_physical_geometry(monitor, &geom);

        _xfw_monitor_set_scale(monitor, manager->scale);
        _xfw_monitor_set_fractional_scale(monitor, manager->scale);
        geom.width /= manager->scale;
        geom.height /= manager->scale;
        _xfw_monitor_set_logical_geometry(monitor, &geom);

        GChecksum *identifier_cksum = g_checksum_new(G_CHECKSUM_SHA1);
        g_checksum_update(identifier_cksum, (guchar *)connector, strlen(connector));
        _xfw_monitor_set_identifier(monitor, g_checksum_get_string(identifier_cksum));
        g_checksum_free(identifier_cksum);

        _xfw_monitor_set_is_primary(monitor, TRUE);

        ensure_workareas(manager);
        update_monitor_workarea(xscreen, monitor, 0);

        GList *monitors = g_list_append(NULL, monitor);
        _xfw_screen_set_monitors(screen, monitors, monitors, NULL);
    } else {
        gdk_x11_register_standard_event_type(display, manager->xrandr_event_base, RRNumberEvents);

        gdk_x11_display_error_trap_push(display);
        XRRSelectInput(dpy,
                       xrootwin,
                       RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputPropertyNotifyMask);
        gdk_x11_display_error_trap_pop_ignored(display);

        refresh_monitors(manager);
    }

    gdk_x11_display_error_trap_push(display);
    XWindowAttributes winattrs;
    XGetWindowAttributes(dpy, xrootwin, &winattrs);
    XSelectInput(dpy, xrootwin, winattrs.your_event_mask | PropertyChangeMask);
    gdk_x11_display_error_trap_pop_ignored(display);
    gdk_window_add_filter(rootwin, rootwin_event_filter, manager);

    return manager;
}

void
_xfw_monitor_manager_x11_destroy(XfwMonitorManagerX11 *manager) {
    if (manager->refresh_idle_id != 0) {
        g_source_remove(manager->refresh_idle_id);
    }

    if (manager->xsettings != NULL) {
        _xsettings_x11_destroy(manager->xsettings);
    }

    GdkScreen *gscreen = _xfw_screen_get_gdk_screen(XFW_SCREEN(manager->screen));
    GdkWindow *rootwin = gdk_screen_get_root_window(gscreen);
    gdk_window_remove_filter(rootwin, rootwin_event_filter, manager);

    g_free(manager);
}

void
_xfw_monitor_x11_workspace_changed(XfwScreenX11 *screen, gint new_workspace_num) {
    update_monitor_workareas_for_workspace(screen, new_workspace_num);
}
