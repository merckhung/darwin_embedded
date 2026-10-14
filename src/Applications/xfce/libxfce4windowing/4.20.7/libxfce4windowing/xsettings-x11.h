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

#ifndef __XSETTINGS_X11_H__
#define __XSETTINGS_X11_H__

#include <gdk/gdk.h>

G_BEGIN_DECLS

typedef struct _XSettingsX11 XSettingsX11;

typedef void (*ScaleFactorChangedFunc)(gint, gpointer);

XSettingsX11 *_xsettings_x11_new(GdkScreen *gscreen,
                                 ScaleFactorChangedFunc scale_factor_changed_func,
                                 gpointer user_data);
gint _xsettings_x11_get_scale(XSettingsX11 *xsettings);
void _xsettings_x11_destroy(XSettingsX11 *xsettings);

G_END_DECLS

#endif /* __XSETTINGS_X11_H__ */
