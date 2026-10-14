/*-
 * Copyright (c) 2003-2006 Benedikt Meurer <benny@xfce.org>
 * Copyright (c) 2010      Ali Abdallah    <aliov@xfce.org>
 * Copyright (c) 2011      Nick Schermer <nick@xfce.org>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA.
 */

#if !defined(_LIBXFCE4UTIL_INSIDE_LIBXFCE4UTIL_H) && !defined(LIBXFCE4UTIL_COMPILATION)
#error "Only <libxfce4util/libxfce4util.h> can be included directly, this file may disappear or change contents"
#endif

#ifndef __XFCE_CONSOLEKIT_H__
#define __XFCE_CONSOLEKIT_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define XFCE_TYPE_CONSOLEKIT (xfce_consolekit_get_type ())
G_DECLARE_FINAL_TYPE (XfceConsolekit, xfce_consolekit, XFCE, CONSOLEKIT, GObject)

XfceConsolekit *
xfce_consolekit_get (void);

gboolean
xfce_consolekit_reboot (XfceConsolekit *consolekit,
                        gboolean polkit_interactive,
                        GError **error);

gboolean
xfce_consolekit_power_off (XfceConsolekit *consolekit,
                           gboolean polkit_interactive,
                           GError **error);

gboolean
xfce_consolekit_suspend (XfceConsolekit *consolekit,
                         gboolean polkit_interactive,
                         GError **error);

gboolean
xfce_consolekit_hibernate (XfceConsolekit *consolekit,
                           gboolean polkit_interactive,
                           GError **error);

gboolean
xfce_consolekit_hybrid_sleep (XfceConsolekit *consolekit,
                              gboolean polkit_interactive,
                              GError **error);

gboolean
xfce_consolekit_can_reboot (XfceConsolekit *consolekit,
                            gboolean *can_reboot,
                            gboolean *auth_reboot,
                            GError **error);

gboolean
xfce_consolekit_can_power_off (XfceConsolekit *consolekit,
                               gboolean *can_power_off,
                               gboolean *auth_power_off,
                               GError **error);

gboolean
xfce_consolekit_can_suspend (XfceConsolekit *consolekit,
                             gboolean *can_suspend,
                             gboolean *auth_suspend,
                             GError **error);

gboolean
xfce_consolekit_can_hibernate (XfceConsolekit *consolekit,
                               gboolean *can_hibernate,
                               gboolean *auth_hibernate,
                               GError **error);

gboolean
xfce_consolekit_can_hybrid_sleep (XfceConsolekit *consolekit,
                                  gboolean *can_hybrid_sleep,
                                  gboolean *auth_hybrid_sleep,
                                  GError **error);

G_END_DECLS

#endif /* !__XFCE_CONSOLEKIT_H__ */
