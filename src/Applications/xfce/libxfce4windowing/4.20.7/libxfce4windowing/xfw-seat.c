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

/**
 * SECTION:xfw-seat
 * @title: XfwSeat
 * @short_description: An object representing a seat
 * @stability: Unstable
 * @include: libxfce4windowing/libxfce4windowing.h
 *
 * #XfwSeat represents a physical monitor, keyboard, and pointing device attached to @screen.
 *
 * Mainly it is only needed when activating #XfwWindow instances.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xfw-seat-private.h"
#include "libxfce4windowing-visibility.h"

#define XFW_SEAT_GET_PRIVATE(seat) ((XfwSeatPrivate *)xfw_seat_get_instance_private(XFW_SEAT(seat)))

typedef struct _XfwSeatPrivate {
    gchar *name;
} XfwSeatPrivate;

enum {
    PROP_0,
    PROP_NAME,
};

static void xfw_seat_set_property(GObject *object,
                                  guint property_id,
                                  const GValue *value,
                                  GParamSpec *pspec);
static void xfw_seat_get_property(GObject *object,
                                  guint property_id,
                                  GValue *value,
                                  GParamSpec *pspec);
static void xfw_seat_finalize(GObject *object);


G_DEFINE_TYPE_WITH_PRIVATE(XfwSeat, xfw_seat, G_TYPE_OBJECT)


static void
xfw_seat_class_init(XfwSeatClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->set_property = xfw_seat_set_property;
    gobject_class->get_property = xfw_seat_get_property;
    gobject_class->finalize = xfw_seat_finalize;

    /**
     * XfwSeat:name
     *
     * The seat's identifier.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_NAME,
                                    g_param_spec_string("name",
                                                        "name",
                                                        "seat name",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_seat_init(XfwSeat *seat) {}


static void
xfw_seat_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec) {
    XfwSeatPrivate *priv = XFW_SEAT_GET_PRIVATE(object);

    switch (property_id) {
        case PROP_NAME:
            g_free(priv->name);
            priv->name = g_value_dup_string(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void
xfw_seat_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec) {
    XfwSeatPrivate *priv = XFW_SEAT_GET_PRIVATE(object);

    switch (property_id) {
        case PROP_NAME:
            g_value_set_string(value, priv->name);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void
xfw_seat_finalize(GObject *object) {
    XfwSeatPrivate *priv = XFW_SEAT_GET_PRIVATE(object);
    g_free(priv->name);
    G_OBJECT_CLASS(xfw_seat_parent_class)->finalize(object);
}

/**
 * xfw_seat_get_name:
 * @seat: an #XfwSeat.
 *
 * Returns the name or identifier of @seat.
 **/
const gchar *
xfw_seat_get_name(XfwSeat *seat) {
    g_return_val_if_fail(XFW_IS_SEAT(seat), NULL);
    return XFW_SEAT_GET_PRIVATE(seat)->name;
}

void
_xfw_seat_set_name(XfwSeat *seat, const gchar *name) {
    g_return_if_fail(XFW_IS_SEAT(seat));

    XfwSeatPrivate *priv = XFW_SEAT_GET_PRIVATE(seat);
    if (g_strcmp0(priv->name, name) != 0) {
        g_free(priv->name);
        priv->name = g_strdup(name);
        g_object_notify(G_OBJECT(seat), "name");
    }
}

#define __XFW_SEAT_C__
#include "libxfce4windowing-visibility.c"
