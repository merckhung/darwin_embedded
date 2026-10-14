/*
 * Copyright (c) 2023,2026 Brian Tarricone <brian@tarricone.org>
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

#ifndef __XFW_WINDOW_ICON_UTILS_H__
#define __XFW_WINDOW_ICON_UTILS_H__

#include <glib.h>

G_BEGIN_DECLS

typedef struct _WindowIcon {
    gint width;
    gint height;
    guchar *bmp;
    gsize bmp_len;
} WindowIcon;

WindowIcon *_window_icon_new(const guint32 *raw_argb32, gint width, gint height, gboolean is_premultiplied);
void _window_icon_free(WindowIcon *window_icon);

gint _window_icon_compare(gconstpointer a, gconstpointer b);

G_END_DECLS

#endif /* __XFW_WINDOW_ICON_UTILS_H__ */
