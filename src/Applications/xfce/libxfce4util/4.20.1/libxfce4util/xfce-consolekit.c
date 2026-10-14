/*-
 * Copyright (c) 2010 Ali Abdallah <aliov@xfce.org>
 * Copyright (c) 2011 Nick Schermer <nick@xfce.org>
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

/**
 * SECTION:xfce-consolekit
 * @title: XfceConsolekit
 * @short_description: Session management via ConsoleKit
 * @include: libxfce4util/libxfce4util.h
 *
 * Since: 4.19.1
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gio/gio.h>

#include "xfce-consolekit.h"
#include "libxfce4util-visibility.h"



static void
xfce_consolekit_finalize (GObject *object);



struct _XfceConsolekit
{
  GObject __parent__;

  GDBusProxy *proxy;
  guint watch_id;
};



G_DEFINE_TYPE (XfceConsolekit, xfce_consolekit, G_TYPE_OBJECT)



static void
xfce_consolekit_class_init (XfceConsolekitClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = xfce_consolekit_finalize;
}



static void
name_appeared (GDBusConnection *connection,
               const gchar *name,
               const gchar *name_owner,
               gpointer user_data)
{
  XfceConsolekit *consolekit = user_data;
  GError *error = NULL;

  g_debug ("%s started up, owned by %s", name, name_owner);

  consolekit->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                     G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                     NULL,
                                                     "org.freedesktop.ConsoleKit",
                                                     "/org/freedesktop/ConsoleKit/Manager",
                                                     "org.freedesktop.ConsoleKit.Manager",
                                                     NULL,
                                                     &error);
  if (error != NULL)
    {
      g_warning ("Failed to get a ConsoleKit proxy: %s", error->message);
      g_error_free (error);
    }
}



static void
name_vanished (GDBusConnection *connection,
               const gchar *name,
               gpointer user_data)
{
  XfceConsolekit *consolekit = user_data;

  g_debug ("%s vanished", name);

  g_clear_object (&consolekit->proxy);
}



static void
xfce_consolekit_init (XfceConsolekit *consolekit)
{
  GError *error = NULL;

  /* try to create a proxy synchronously with default flags, so the returned
   * object is immediately usable */
  consolekit->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     NULL,
                                                     "org.freedesktop.ConsoleKit",
                                                     "/org/freedesktop/ConsoleKit/Manager",
                                                     "org.freedesktop.ConsoleKit.Manager",
                                                     NULL,
                                                     &error);
  if (error != NULL)
    {
      g_warning ("Failed to get a ConsoleKit proxy: %s", error->message);
      g_error_free (error);

      /* watch the name if it failed, maybe we'll have better luck later */
      consolekit->watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                               "org.freedesktop.ConsoleKit",
                                               G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                               name_appeared,
                                               name_vanished,
                                               consolekit,
                                               NULL);
    }
}



static void
xfce_consolekit_finalize (GObject *object)
{
  XfceConsolekit *consolekit = XFCE_CONSOLEKIT (object);

  if (consolekit->watch_id != 0)
    g_bus_unwatch_name (consolekit->watch_id);
  g_clear_object (&consolekit->proxy);

  G_OBJECT_CLASS (xfce_consolekit_parent_class)->finalize (object);
}



static gboolean
xfce_consolekit_can_method_old (XfceConsolekit *consolekit,
                                const gchar *method,
                                gboolean *can_method_out,
                                gboolean *auth_method_out,
                                GError **error)
{
  GVariant *variant;
  gboolean can_method;

  /* never return true if something fails */
  if (can_method_out != NULL)
    *can_method_out = FALSE;
  if (auth_method_out != NULL)
    *auth_method_out = FALSE;

  if (consolekit->proxy == NULL)
    {
      g_debug ("No ConsoleKit proxy");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No ConsoleKit proxy available");
      return FALSE;
    }

  g_debug ("Calling %s", method);

  variant = g_dbus_proxy_call_sync (consolekit->proxy,
                                    method,
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1,
                                    NULL,
                                    error);
  if (variant == NULL)
    return FALSE;

  g_variant_get_child (variant, 0, "b", &can_method);
  g_variant_unref (variant);

  if (can_method_out != NULL)
    *can_method_out = can_method;
  if (auth_method_out != NULL)
    *auth_method_out = can_method;

  return TRUE;
}



static gboolean
xfce_consolekit_can_method (XfceConsolekit *consolekit,
                            const gchar *method,
                            gboolean *can_method_out,
                            gboolean *auth_method_out,
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

  if (consolekit->proxy == NULL)
    {
      g_debug ("No ConsoleKit proxy");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No ConsoleKit proxy available");
      return FALSE;
    }

  g_debug ("Calling %s", method);

  variant = g_dbus_proxy_call_sync (consolekit->proxy,
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
xfce_consolekit_method_old (XfceConsolekit *consolekit,
                            const gchar *method,
                            GError **error)
{
  GVariant *variant;

  if (consolekit->proxy == NULL)
    {
      g_debug ("No ConsoleKit proxy");
      return FALSE;
    }

  g_debug ("Calling %s", method);

  variant = g_dbus_proxy_call_sync (consolekit->proxy,
                                    method,
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1,
                                    NULL,
                                    error);
  if (variant == NULL)
    return FALSE;

  g_variant_unref (variant);

  return TRUE;
}



static gboolean
xfce_consolekit_method (XfceConsolekit *consolekit,
                        const gchar *method,
                        gboolean polkit_interactive,
                        GError **error)
{
  GVariant *variant;

  if (consolekit->proxy == NULL)
    {
      g_debug ("No ConsoleKit proxy");
      return FALSE;
    }

  g_debug ("Calling %s", method);

  variant = g_dbus_proxy_call_sync (consolekit->proxy,
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
 * xfce_consolekit_get:
 *
 * Create a new #XfceConsolekit instance or increase reference count.
 *
 * Returns: (transfer full): A reference to the singleton object, to be released with g_object_unref().
 *
 * Since: 4.19.1
 **/
XfceConsolekit *
xfce_consolekit_get (void)
{
  static XfceConsolekit *object = NULL;

  if (G_LIKELY (object != NULL))
    {
      g_object_ref (G_OBJECT (object));
    }
  else
    {
      object = g_object_new (XFCE_TYPE_CONSOLEKIT, NULL);
      g_object_add_weak_pointer (G_OBJECT (object), (gpointer) &object);
    }

  return object;
}



/**
 * xfce_consolekit_reboot:
 * @consolekit: the #XfceConsolekit object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask ConsoleKit to trigger Reboot.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_reboot (XfceConsolekit *consolekit,
                        gboolean polkit_interactive,
                        GError **error)
{
  GError *local_error = NULL;

  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!xfce_consolekit_method (consolekit, "Reboot", polkit_interactive, &local_error))
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        {
          g_error_free (local_error);
          return xfce_consolekit_method_old (consolekit, "Restart", error);
        }

      g_propagate_error (error, local_error);
      return FALSE;
    }

  return TRUE;
}



/**
 * xfce_consolekit_power_off:
 * @consolekit: the #XfceConsolekit object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask ConsoleKit to trigger PowerOff.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_power_off (XfceConsolekit *consolekit,
                           gboolean polkit_interactive,
                           GError **error)
{
  GError *local_error = NULL;

  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!xfce_consolekit_method (consolekit, "PowerOff", polkit_interactive, &local_error))
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        {
          g_error_free (local_error);
          return xfce_consolekit_method_old (consolekit, "Stop", error);
        }

      g_propagate_error (error, local_error);
      return FALSE;
    }

  return TRUE;
}



/**
 * xfce_consolekit_suspend:
 * @consolekit: the #XfceConsolekit object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask ConsoleKit to trigger Suspend.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_suspend (XfceConsolekit *consolekit,
                         gboolean polkit_interactive,
                         GError **error)
{
  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_consolekit_method (consolekit, "Suspend", polkit_interactive, error);
}



/**
 * xfce_consolekit_hibernate:
 * @consolekit: the #XfceConsolekit object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask ConsoleKit to trigger Hibernate.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_hibernate (XfceConsolekit *consolekit,
                           gboolean polkit_interactive,
                           GError **error)
{
  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_consolekit_method (consolekit, "Hibernate", polkit_interactive, error);
}



/**
 * xfce_consolekit_hybrid_sleep:
 * @consolekit: the #XfceConsolekit object
 * @polkit_interactive: whether PolicyKit should ask the user to authenticate if needed
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Ask ConsoleKit to trigger HybridSleep.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_hybrid_sleep (XfceConsolekit *consolekit,
                              gboolean polkit_interactive,
                              GError **error)
{
  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_consolekit_method (consolekit, "HybridSleep", polkit_interactive, error);
}



/**
 * xfce_consolekit_can_reboot:
 * @consolekit: the #XfceConsolekit object
 * @can_reboot: (out) (nullable): location to store capacity or %NULL
 * @auth_reboot: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether ConsoleKit can trigger Reboot.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_can_reboot (XfceConsolekit *consolekit,
                            gboolean *can_reboot,
                            gboolean *auth_reboot,
                            GError **error)
{
  GError *local_error = NULL;

  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!xfce_consolekit_can_method (consolekit, "CanReboot", can_reboot, auth_reboot, &local_error))
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        {
          g_error_free (local_error);
          return xfce_consolekit_can_method_old (consolekit, "CanRestart", can_reboot, auth_reboot, error);
        }

      g_propagate_error (error, local_error);
      return FALSE;
    }

  return TRUE;
}



/**
 * xfce_consolekit_can_power_off:
 * @consolekit: the #XfceConsolekit object
 * @can_power_off: (out) (nullable): location to store capacity or %NULL
 * @auth_power_off: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether ConsoleKit can trigger PowerOff.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_can_power_off (XfceConsolekit *consolekit,
                               gboolean *can_power_off,
                               gboolean *auth_power_off,
                               GError **error)
{
  GError *local_error = NULL;

  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!xfce_consolekit_can_method (consolekit, "CanPowerOff", can_power_off, auth_power_off, &local_error))
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        {
          g_error_free (local_error);
          return xfce_consolekit_can_method_old (consolekit, "CanStop", can_power_off, auth_power_off, error);
        }

      g_propagate_error (error, local_error);
      return FALSE;
    }

  return TRUE;
}



/**
 * xfce_consolekit_can_suspend:
 * @consolekit: the #XfceConsolekit object
 * @can_suspend: (out) (nullable): location to store capacity or %NULL
 * @auth_suspend: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether ConsoleKit can trigger and has authorization for Suspend.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_can_suspend (XfceConsolekit *consolekit,
                             gboolean *can_suspend,
                             gboolean *auth_suspend,
                             GError **error)
{
  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_consolekit_can_method (consolekit, "CanSuspend", can_suspend, auth_suspend, error);
}



/**
 * xfce_consolekit_can_hibernate:
 * @consolekit: the #XfceConsolekit object
 * @can_hibernate: (out) (nullable): location to store capacity or %NULL
 * @auth_hibernate: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether ConsoleKit can trigger and has authorization for Hibernate.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_can_hibernate (XfceConsolekit *consolekit,
                               gboolean *can_hibernate,
                               gboolean *auth_hibernate,
                               GError **error)
{
  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_consolekit_can_method (consolekit, "CanHibernate", can_hibernate, auth_hibernate, error);
}



/**
 * xfce_consolekit_can_hybrid_sleep:
 * @consolekit: the #XfceConsolekit object
 * @can_hybrid_sleep: (out) (nullable): location to store capacity or %NULL
 * @auth_hybrid_sleep: (out) (nullable): location to store authorization or %NULL
 * @error: (out) (nullable): location to store error on failure or %NULL
 *
 * Check whether ConsoleKit can trigger and has authorization for HybridSleep.
 *
 * Returns: %TRUE if the D-Bus request was successful, %FALSE otherwise and @error is set.
 *
 * Since: 4.19.1
 **/
gboolean
xfce_consolekit_can_hybrid_sleep (XfceConsolekit *consolekit,
                                  gboolean *can_hybrid_sleep,
                                  gboolean *auth_hybrid_sleep,
                                  GError **error)
{
  g_return_val_if_fail (XFCE_IS_CONSOLEKIT (consolekit), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return xfce_consolekit_can_method (consolekit, "CanHybridSleep", can_hybrid_sleep, auth_hybrid_sleep, error);
}

#define __XFCE_CONSOLEKIT_C__
#include "libxfce4util-visibility.c"
