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

#ifndef __XFW_SEAT_PRIVATE_H__
#define __XFW_SEAT_PRIVATE_H__

#include <glib-object.h>

#include "xfw-seat.h"

G_BEGIN_DECLS

struct _XfwSeatClass {
    GObjectClass parent_class;
};

void _xfw_seat_set_name(XfwSeat *seat, const gchar *name);

G_END_DECLS

#endif /* __XFW_SEAT_PRIVATE_H__ */
