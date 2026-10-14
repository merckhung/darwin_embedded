/*
 * Copyright (c) 2022 Brian Tarricone <brian@tarricone.org>
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

#ifndef __XFW_WINDOW_ACTION_MENU_H__
#define __XFW_WINDOW_ACTION_MENU_H__

#include <gtk/gtk.h>
#include <libxfce4windowing/libxfce4windowing.h>

G_BEGIN_DECLS

#define XFW_TYPE_WINDOW_ACTION_MENU (xfw_window_action_menu_get_type())
G_DECLARE_FINAL_TYPE(XfwWindowActionMenu, xfw_window_action_menu, XFW, WINDOW_ACTION_MENU, GtkMenu)

typedef struct _XfwWindowActionMenuPrivate XfwWindowActionMenuPrivate;

GtkWidget *xfw_window_action_menu_new(XfwWindow *window);

G_END_DECLS

#endif /* !__XFW_WINDOW_ACTION_MENU_H__ */
