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
#include <X11/Xmd.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>

#include "xsettings-x11.h"

#define XSETTINGS_PAD(n, m) ((n + m - 1) & (~(m - 1)))

struct _XSettingsX11 {
    ScaleFactorChangedFunc scale_factor_changed_func;
    gpointer user_data;

    GdkDisplay *display;
    GdkWindow *rootwin;
    Atom manager_atom;
    GdkWindow *gdkwin;

    gint scale;
};

enum {
    XSettingsTypeInteger = 0,
    XSettingsTypeString = 1,
    XSettingsTypeColor = 2,
};

static void
my_xfree(gpointer data) {
    XFree(data);
}

static gboolean
read_card8(GInputStream *is, guint8 *dest) {
    return g_input_stream_read(is, dest, sizeof(*dest), NULL, NULL) == sizeof(*dest);
}

static gboolean
read_card16(GInputStream *is, CARD8 byte_order, guint16 *dest) {
    CARD16 card16;
    if (g_input_stream_read(is, &card16, sizeof(card16), NULL, NULL) != sizeof(card16)) {
        return FALSE;
    } else if (byte_order == MSBFirst) {
        *dest = GUINT16_FROM_BE(card16);
    } else {
        *dest = GUINT16_FROM_LE(card16);
    }
    return TRUE;
}

static gboolean
read_card32(GInputStream *is, CARD8 byte_order, guint32 *dest) {
    CARD32 card32;
    if (g_input_stream_read(is, &card32, sizeof(card32), NULL, NULL) != sizeof(card32)) {
        return FALSE;
    } else if (byte_order == MSBFirst) {
        *dest = GUINT32_FROM_BE(card32);
    } else {
        *dest = GUINT32_FROM_LE(card32);
    }
    return TRUE;
}

static gboolean
read_string(GInputStream *is, guint32 len, gchar **dest) {
    guint pad = XSETTINGS_PAD(len, 4);
    if (pad < len) {
        // invalid; overflow
        return FALSE;
    }

    void *buf = g_malloc(pad + 1);
    if (g_input_stream_read(is, buf, pad, NULL, NULL) == pad) {
        *dest = (gchar *)buf;
        (*dest)[len] = '\0';
        return TRUE;
    } else {
        g_free(buf);
        return FALSE;
    }
}

static gboolean
skip_string(GInputStream *is, guint len) {
    guint pad = XSETTINGS_PAD(len, 4);
    if (pad < len) {
        // invalid; overflow
        return FALSE;
    }

    return g_input_stream_skip(is, pad, NULL, NULL) == pad;
}

static gboolean
update_scale_xsetting(XSettingsX11 *xsettings) {
    gboolean needs_notify = FALSE;
    Display *dpy = gdk_x11_display_get_xdisplay(xsettings->display);
    Atom xsettings_atom = XInternAtom(dpy, "_XSETTINGS_SETTINGS", False);
    Atom actual_type;
    int actual_format;
    gulong nitems = 0;
    gulong bytes_after = 0;
    guchar *prop = NULL;

    gdk_x11_display_error_trap_push(xsettings->display);
    int ret = XGetWindowProperty(dpy,
                                 gdk_x11_window_get_xid(xsettings->gdkwin),
                                 xsettings_atom,
                                 0,
                                 LONG_MAX,
                                 False,
                                 xsettings_atom,
                                 &actual_type,
                                 &actual_format,
                                 &nitems,
                                 &bytes_after,
                                 &prop);
    if (gdk_x11_display_error_trap_pop(xsettings->display) != 0
        || ret != Success
        || actual_type != xsettings_atom
        || actual_format != 8)
    {
        g_clear_pointer(&prop, XFree);
    } else {
        GInputStream *is = g_memory_input_stream_new_from_data(prop, nitems, my_xfree);

        CARD8 byte_order = 0;
        guint32 n_settings = 0;

        if (g_input_stream_read(is, &byte_order, 1, NULL, NULL) != 1
            || (byte_order != MSBFirst && byte_order != LSBFirst)
            || g_input_stream_skip(is, 3, NULL, NULL) != 3  // unused bytes
            || g_input_stream_skip(is, sizeof(CARD32), NULL, NULL) != sizeof(CARD32)  // serial
            || !read_card32(is, byte_order, &n_settings))
        {
            g_message("Failed to read XSETTINGS header");
        } else {
            for (guint32 i = 0; i < n_settings; ++i) {
                guint8 type = 0;
                guint16 name_len = 0;

                if (!read_card8(is, &type)
                    || g_input_stream_skip(is, 1, NULL, NULL) != 1  // unused byte
                    || !read_card16(is, byte_order, &name_len))
                {
                    g_message("Failed to read XSETTINGS setting at position %u", i);
                    break;
                } else {
                    if (type == XSettingsTypeInteger) {
                        gchar *name = NULL;

                        if (!read_string(is, name_len, &name)) {
                            g_message("Failed to read name of XSETTINGS integer setting at position %u", i);
                            break;
                        } else {
                            gboolean is_scaling_factor = g_strcmp0(name, "Gdk/WindowScalingFactor") == 0;
                            g_free(name);

                            if (is_scaling_factor) {
                                guint32 value = 0;

                                if (g_input_stream_skip(is, sizeof(CARD32), NULL, NULL) == sizeof(CARD32)  // last-change-serial
                                    && read_card32(is, byte_order, &value))  // value
                                {
                                    if (xsettings->scale != (gint)value) {
                                        xsettings->scale = value;
                                        needs_notify = TRUE;
                                    }
                                } else {
                                    g_message("Failed to read XSETTINGS integer setting at position %u", i);
                                }
                                break;
                            } else if (g_input_stream_skip(is, sizeof(CARD32), NULL, NULL) != sizeof(CARD32)  // last-change-serial
                                       || g_input_stream_skip(is, sizeof(CARD32), NULL, NULL) != sizeof(CARD32))  // value
                            {
                                g_message("Failed to skip XSETTINGS integer setting at position %u", i);
                                break;
                            }
                        }
                    } else if (type == XSettingsTypeColor) {
                        if (!skip_string(is, name_len)  // name
                            || g_input_stream_skip(is, sizeof(CARD32), NULL, NULL) != sizeof(CARD32)  // last-change-serial
                            || g_input_stream_skip(is, sizeof(CARD16) * 4, NULL, NULL) != sizeof(CARD16) * 4)  // color values
                        {
                            g_message("Failed to skip XSETTINGS color setting at position %u", i);
                            break;
                        }
                    } else if (type == XSettingsTypeString) {
                        guint32 value_len = 0;

                        if (!skip_string(is, name_len)  // name
                            || g_input_stream_skip(is, sizeof(CARD32), NULL, NULL) != sizeof(CARD32)  // last-change-serial
                            || !read_card32(is, byte_order, &value_len)  // value length
                            || !skip_string(is, value_len))  // value
                        {
                            g_message("Failed to skip XSETTINGS string setting at position %u", i);
                            break;
                        }
                    } else {
                        g_message("Invalid XSETTINGS setting type %u at position %u", type, i);
                        break;
                    }
                }
            }
        }

        g_object_unref(is);
    }

    return needs_notify;
}

static GdkFilterReturn
xsettings_window_filter(GdkXEvent *gdkxevent, GdkEvent *event, gpointer data) {
    XSettingsX11 *xsettings = data;
    XEvent *xevent = (XEvent *)gdkxevent;

    if (xevent->xany.window == gdk_x11_window_get_xid(xsettings->gdkwin)) {
        if (xevent->type == DestroyNotify) {
            gdk_window_remove_filter(NULL, xsettings_window_filter, xsettings);
            g_clear_object(&xsettings->gdkwin);
        } else if (xevent->type == PropertyNotify
                   && xevent->xproperty.atom == XInternAtom(xevent->xproperty.display, "_XSETTINGS_SETTINGS", False))
        {
            if (update_scale_xsetting(xsettings)) {
                xsettings->scale_factor_changed_func(xsettings->scale, xsettings->user_data);
            }
        }
    }

    return GDK_FILTER_CONTINUE;
}

static void
get_manager_selection(XSettingsX11 *xsettings, gboolean do_notify) {
    if (xsettings->gdkwin != NULL) {
        gdk_window_remove_filter(NULL, xsettings_window_filter, xsettings);
        g_clear_object(&xsettings->gdkwin);
    }
    gdk_x11_display_error_trap_push(xsettings->display);
    gdk_x11_display_grab(xsettings->display);

    Display *dpy = gdk_x11_display_get_xdisplay(xsettings->display);
    Window win = XGetSelectionOwner(dpy, xsettings->manager_atom);
    if (win != None) {
        xsettings->gdkwin = gdk_x11_window_foreign_new_for_display(xsettings->display, win);
        if (xsettings->gdkwin == NULL) {
            g_message("Failed to wrap XSETTINGS window");
        } else {
            XSelectInput(dpy, gdk_x11_window_get_xid(xsettings->gdkwin), StructureNotifyMask | PropertyChangeMask);
        }
    }

    gdk_x11_display_ungrab(xsettings->display);
    gdk_display_flush(xsettings->display);
    if (gdk_x11_display_error_trap_pop(xsettings->display) != 0) {
        g_message("Errors encountered while finding XSETTINGS manager");
    }

    gboolean needs_notify;
    if (xsettings->gdkwin != NULL) {
        // GDK annoyingly returns GDK_FILTER_REMOVE when it sees PropertyNotify
        // on the XSETTINGS window, which causes us to never see changes.  What
        // we can do instead is add a "global" filter that gets called for all
        // events, regardless of window.  These filters get called before the
        // per-window filters.
        gdk_window_add_filter(NULL, xsettings_window_filter, xsettings);
        needs_notify = update_scale_xsetting(xsettings);
    } else {
        needs_notify = FALSE;
    }

    if (needs_notify && do_notify) {
        xsettings->scale_factor_changed_func(xsettings->scale, xsettings->user_data);
    }
}

static GdkFilterReturn
rootwin_filter(GdkXEvent *gdkxevent, GdkEvent *event, gpointer data) {
    XSettingsX11 *xsettings = data;
    XEvent *xevent = (XEvent *)gdkxevent;

    if (xevent->type == ClientMessage
        && xevent->xclient.window == gdk_x11_window_get_xid(xsettings->rootwin)
        && xevent->xclient.message_type == XInternAtom(xevent->xclient.display, "MANAGER", False)
        && xevent->xclient.format == 32
        && (Atom)xevent->xclient.data.l[1] == xsettings->manager_atom)
    {
        get_manager_selection(xsettings, TRUE);
    }

    return GDK_FILTER_CONTINUE;
}


XSettingsX11 *
_xsettings_x11_new(GdkScreen *gscreen, ScaleFactorChangedFunc scale_factor_changed_func, gpointer user_data) {
    XSettingsX11 *xsettings = g_new0(XSettingsX11, 1);
    xsettings->display = gdk_screen_get_display(gscreen);
    xsettings->scale_factor_changed_func = scale_factor_changed_func;
    xsettings->user_data = user_data;
    xsettings->scale = 1;

    Display *dpy = gdk_x11_display_get_xdisplay(gdk_screen_get_display(gscreen));
    xsettings->rootwin = gdk_screen_get_root_window(gscreen);
    Window xrootwin = gdk_x11_window_get_xid(xsettings->rootwin);

    int screen_num = gdk_x11_screen_get_screen_number(gscreen);
    gchar *xsettings_manager_atom_name = g_strdup_printf("_XSETTINGS_S%d", screen_num);
    xsettings->manager_atom = XInternAtom(dpy, xsettings_manager_atom_name, False);
    g_free(xsettings_manager_atom_name);

    gdk_x11_display_error_trap_push(xsettings->display);
    XWindowAttributes attrs;
    XGetWindowAttributes(dpy, xrootwin, &attrs);
    XSelectInput(dpy, xrootwin, attrs.your_event_mask | StructureNotifyMask);
    gdk_x11_display_error_trap_pop_ignored(xsettings->display);

    // GDK annoyingly returns GDK_FILTER_REMOVE when it sees the ClientMessage
    // for the XSETTINGS manager, which causes us to never see changes.  What
    // we can do instead is add a "global" filter that gets called for all
    // events, regardless of window.  These filters get called before the
    // per-window filters.
    gdk_window_add_filter(NULL, rootwin_filter, xsettings);

    get_manager_selection(xsettings, FALSE);

    return xsettings;
}

gint
_xsettings_x11_get_scale(XSettingsX11 *xsettings) {
    return xsettings->scale;
}

void
_xsettings_x11_destroy(XSettingsX11 *xsettings) {
    if (xsettings->gdkwin != NULL) {
        gdk_window_remove_filter(NULL, xsettings_window_filter, xsettings);
        g_object_unref(xsettings->gdkwin);
    }

    gdk_window_remove_filter(NULL, rootwin_filter, xsettings);

    g_free(xsettings);
}
