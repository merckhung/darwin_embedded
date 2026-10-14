/*-
 * Copyright (C) 2012 Christian Hesse
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

/**
 * SECTION:xfce-systemd
 * @title: XfceSystemd
 * @short_description: Session management via systemd
 * @include: libxfce4util/libxfce4util.h
 *
 * Since: 4.19.1
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gio/gio.h>

#include "xfce-systemd.h"
#include "libxfce4util-visibility.h"



static void
xfce_systemd_finalize (GObject *object);



struct _XfceSystemd
{
  GObject __parent__;

  GDBusProxy *proxy;
  guint watch_id;
};



G_DEFINE_TYPE (XfceSystemd, xfce_systemd, G_TYPE_OBJECT)



static void
xfce_systemd_class_init (XfceSystemdClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = xfce_systemd_finalize;
}



static void
name_appeared (GDBusConnection *connection,
               const gchar *name,
               const gchar *name_owner,
               gpointer user_data)
{
  XfceSystemd *systemd = user_data;
  GError *error = NULL;

  g_debug ("%s started up, owned by %s", name, name_owner);

  systemd->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                  NULL,
                                                  "org.freedesktop.login1",
                                                  "/org/freedesktop/login1",
                                                  "org.freedesktop.login1.Manager",
                                                  NULL,
                                                  &error);
  if (error != NULL)
    {
      g_warning ("Failed to get a systemd proxy: %s", error->message);
      g_error_free (error);
    }
}



static void
name_vanished (GDBusConnection *connection,
               const gchar *name,
               gpointer user_data)
{
  XfceSystemd *systemd = user_data;

  g_debug ("%s vanished", name);

  g_clear_object (&systemd->proxy);
}



static void
xfce_systemd_init (XfceSystemd *systemd)
{
  GError *error = NULL;

  /* try to create a proxy synchronously with default flags, so the returned
   * object is immediately usable */
  systemd->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  NULL,
                                                  "org.freedesktop.login1",
                                                  "/org/freedesktop/login1",
                                                  "org.freedesktop.login1.Manager",
                                                  NULL,
                                                  &error);
  if (error != NULL)
    {
      g_warning ("Failed to get a systemd proxy: %s", error->message);
      g_error_free (error);

      /* watch the name if it failed, maybe we'll have better luck later */
      systemd->watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                            "org.freedesktop.login1",
                                            G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                            name_appeared,
                                            name_vanished,
                                            systemd,
                                            NULL);
    }
}



static void
xfce_systemd_finalize (GObject *object)
{
  XfceSystemd *systemd = XFCE_SYSTEMD (object);

  if (systemd->watch_id != 0)
    g_bus_unwatch_name (systemd->watch_id);
  g_clear_object (&systemd->proxy);

  G_OBJECT_CLASS (xfce_systemd_parent_class)->finalize (object);
}



static gboolean
xfce_systemd_can_method (XfceSystemd *systemd,
                         gboolean *can_method_out,
                         gboolean *auth_method_out,
                         const gchar *method,
                         GError **error)
{
  GVariant *variant;
  const gchar *can_string;
  gboolean can_method;
  gboolean auth_method;

  /* never return true if something fails */
  if (can_method_out != NULL)
    *can_method_out = FALSE;
  if (auth_method_out != NULL)
    *auth_method_out = FALSE;

  if (systemd->proxy == NULL)
    {
      g_debug ("No systemd proxy");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No systemd proxy available");
      return FALSE;
    }

  g_debug ("Calling %s", method);

  variant = g_dbus_proxy_call_sync (systemd->proxy,
                                    method,
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1,
                                    NULL,
                                    error);
  if (variant == NULL)
    return FALSE;

  g_variant_get_child (variant, 0, "&s", &can_string);
  can_method = g_strcmp0 (can_string, "na") != 0;
  auth_method = g_strcmp0 (can_string, "yes") == 0 || g_strcmp0 (can_string, "challenge") == 0;
  g_variant_unref (variant);

  if (can_method_out != NULL)
    *can_method_out = can_method;
  if (auth_method_out != NULL)
    *auth_method_out = auth_method;

  return TRUE;
}



static gboolean
xfce_systemd_method (XfceSystemd *systemd,
                     const gchar *method,
                     gboolean polkit_interactive,
                     GError **error)
{
  GVariant *variant;

  if (systemd->proxy == NULL)
    {
      g_debug ("No systemd proxy");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No systemd proxy available");
      return FALSE;
    }

  g_debug ("Calling %s", method);

  variant = g_dbus_proxy_call_sync (systemd->proxy,
                                    method,
                                    g_variant_new ("(b)", polkit_interactive),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1,
                                    NULL,
                                    error);
  if (variant == NULL)
    return FALSE;

  g_variant_unref (variant);

  return TRUE;
}



/**
 * xfce_systemd_get:
 *
 * Create a new #XfceSystemd instance or increase reference count.
 *
 * Returns: (transfer full): A reference to the singleton object, to be released with g_object_unref().
 *
 * Since: 4.19.1
 **/
XfceSystemd *
xfce_systemd_get (void)
{
  static XfceSystemd *object = NULL;

  if (G_LIKELY (object != NULL))
    {
      g_object_ref (G_OBJECT (object));
    }
  else
    {
      object = g_object_new (XFCE_TYPE_SYSTEMD, NULL);
      g_object_add_weak_pointer (G_OBJECT (object), (gpointer) &object);
    }

  return object;
}



/**
 * xfce_systemd_reboot:
 * @systemd: the #XfceSystemd object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask systemd to trigger Reboot.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_reboot (XfceSystemd *systemd,
                     gboolean polkit_interactive,
                     GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_method (systemd, "Reboot", polkit_interactive, error);
}



/**
 * xfce_systemd_power_off:
 * @systemd: the #XfceSystemd object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask systemd to trigger PowerOff.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_power_off (XfceSystemd *systemd,
                        gboolean polkit_interactive,
                        GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_method (systemd, "PowerOff", polkit_interactive, error);
}



/**
 * xfce_systemd_suspend:
 * @systemd: the #XfceSystemd object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask systemd to trigger Suspend.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_suspend (XfceSystemd *systemd,
                      gboolean polkit_interactive,
                      GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_method (systemd, "Suspend", polkit_interactive, error);
}



/**
 * xfce_systemd_hibernate:
 * @systemd: the #XfceSystemd object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask systemd to trigger Hibernate.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_hibernate (XfceSystemd *systemd,
                        gboolean polkit_interactive,
                        GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_method (systemd, "Hibernate", polkit_interactive, error);
}



/**
 * xfce_systemd_hybrid_sleep:
 * @systemd: the #XfceSystemd object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask systemd to trigger HybridSleep.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_hybrid_sleep (XfceSystemd *systemd,
                           gboolean polkit_interactive,
                           GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_method (systemd, "HybridSleep", polkit_interactive, error);
}



/**
 * xfce_systemd_can_reboot:
 * @systemd: the #XfceSystemd object
 * @can_reboot: (out) (nullable): location to store capacity or %NULL
 * @auth_reboot: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether systemd can trigger Reboot.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_can_reboot (XfceSystemd *systemd,
                         gboolean *can_reboot,
                         gboolean *auth_reboot,
                         GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_can_method (systemd, can_reboot, auth_reboot, "CanReboot", error);
}



/**
 * xfce_systemd_can_power_off:
 * @systemd: the #XfceSystemd object
 * @can_power_off: (out) (nullable): location to store capacity or %NULL
 * @auth_power_off: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether systemd can trigger PowerOff.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_can_power_off (XfceSystemd *systemd,
                            gboolean *can_power_off,
                            gboolean *auth_power_off,
                            GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_can_method (systemd, can_power_off, auth_power_off, "CanPowerOff", error);
}



/**
 * xfce_systemd_can_suspend:
 * @systemd: the #XfceSystemd object
 * @can_suspend: (out) (nullable): location to store capacity or %NULL
 * @auth_suspend: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether systemd can trigger and has authorization for Suspend.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_can_suspend (XfceSystemd *systemd,
                          gboolean *can_suspend,
                          gboolean *auth_suspend,
                          GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_can_method (systemd, can_suspend, auth_suspend, "CanSuspend", error);
}



/**
 * xfce_systemd_can_hibernate:
 * @systemd: the #XfceSystemd object
 * @can_hibernate: (out) (nullable): location to store capacity or %NULL
 * @auth_hibernate: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether systemd can trigger and has authorization for Hibernate.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_can_hibernate (XfceSystemd *systemd,
                            gboolean *can_hibernate,
                            gboolean *auth_hibernate,
                            GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_can_method (systemd, can_hibernate, auth_hibernate, "CanHibernate", error);
}



/**
 * xfce_systemd_can_hybrid_sleep:
 * @systemd: the #XfceSystemd object
 * @can_hybrid_sleep: (out) (nullable): location to store capacity or %NULL
 * @auth_hybrid_sleep: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether systemd can trigger and has authorization for HybridSleep.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_systemd_can_hybrid_sleep (XfceSystemd *systemd,
                               gboolean *can_hybrid_sleep,
                               gboolean *auth_hybrid_sleep,
                               GError **error)
{
  g_return_val_if_fail (XFCE_IS_SYSTEMD (systemd), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_systemd_can_method (systemd, can_hybrid_sleep, auth_hybrid_sleep, "CanHybridSleep", error);
}

#define __XFCE_SYSTEMD_C__
#include "libxfce4util-visibility.c"
