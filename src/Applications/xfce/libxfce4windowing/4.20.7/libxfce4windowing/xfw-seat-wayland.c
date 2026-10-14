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

#include <wayland-client-protocol.h>

#include "xfw-screen-wayland.h"
#include "xfw-seat-wayland.h"

struct _XfwSeatWayland {
    XfwSeat parent;

    XfwScreenWayland *screen;
    struct wl_seat *wl_seat;
    uint32_t global_name;
};

static void xfw_seat_wayland_finalize(GObject *object);

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name);
static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities);


G_DEFINE_FINAL_TYPE(XfwSeatWayland, xfw_seat_wayland, XFW_TYPE_SEAT)


static const struct wl_seat_listener seat_listener = {
    .name = seat_name,
    .capabilities = seat_capabilities,
};

static void
xfw_seat_wayland_class_init(XfwSeatWaylandClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->finalize = xfw_seat_wayland_finalize;
}

static void
xfw_seat_wayland_init(XfwSeatWayland *seat) {}

static void
xfw_seat_wayland_finalize(GObject *object) {
    XfwSeatWayland *seat = XFW_SEAT_WAYLAND(object);
    wl_seat_destroy(seat->wl_seat);
    G_OBJECT_CLASS(xfw_seat_wayland_parent_class)->finalize(object);
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    XfwSeat *seat = XFW_SEAT(data);
    gboolean was_null = xfw_seat_get_name(seat) == NULL;
    _xfw_seat_set_name(seat, name);
    if (was_null) {
        XfwSeatWayland *wseat = XFW_SEAT_WAYLAND(seat);
        _xfw_screen_wayland_seat_ready(wseat->screen, wseat);
    }
}

static void
seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {}

XfwSeatWayland *
_xfw_seat_wayland_new(XfwScreen *screen, struct wl_seat *wl_seat, uint32_t global_name) {
    g_return_val_if_fail(XFW_IS_SCREEN_WAYLAND(screen), NULL);
    g_return_val_if_fail(wl_seat != NULL, NULL);

    XfwSeatWayland *seat = g_object_new(XFW_TYPE_SEAT_WAYLAND, NULL);
    seat->screen = XFW_SCREEN_WAYLAND(screen);
    seat->wl_seat = wl_seat;
    seat->global_name = global_name;
    wl_seat_add_listener(wl_seat, &seat_listener, seat);

    return seat;
}

struct wl_seat *
_xfw_seat_wayland_get_wl_seat(XfwSeatWayland *seat) {
    g_return_val_if_fail(XFW_IS_SEAT_WAYLAND(seat), NULL);
    return seat->wl_seat;
}

uint32_t
_xfw_seat_wayland_get_global_name(XfwSeatWayland *seat) {
    g_return_val_if_fail(XFW_IS_SEAT_WAYLAND(seat), 0);
    return seat->global_name;
}
