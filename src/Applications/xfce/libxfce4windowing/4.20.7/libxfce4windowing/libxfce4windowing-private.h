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

#ifndef __LIBXFCE4WINDOWING_PRIVATE_H__
#define __LIBXFCE4WINDOWING_PRIVATE_H__

#ifdef ENABLE_X11
#include <X11/Xlib.h>
#endif

#include <gdk/gdk.h>
#include <gio/gdesktopappinfo.h>
#include <glib-object.h>

// Support glib < 2.74, avoid deprecation warnings when building with xfce4-dev-tools >= 4.17.1

#ifdef G_DEFINE_ENUM_TYPE
#undef G_DEFINE_ENUM_TYPE
#endif
#define G_DEFINE_ENUM_TYPE(TypeName, type_name, ...) \
    GType \
        type_name##_get_type(void) { \
        static gsize g_define_type__static = 0; \
        if (g_once_init_enter(&g_define_type__static)) { \
            static const GEnumValue enum_values[] = { \
                __VA_ARGS__, \
                { 0, NULL, NULL }, \
            }; \
            GType g_define_type = g_enum_register_static(g_intern_static_string(#TypeName), enum_values); \
            g_once_init_leave(&g_define_type__static, g_define_type); \
        } \
        return g_define_type__static; \
    }

#ifdef G_DEFINE_FLAGS_TYPE
#undef G_DEFINE_FLAGS_TYPE
#endif
#define G_DEFINE_FLAGS_TYPE(TypeName, type_name, ...) \
    GType \
        type_name##_get_type(void) { \
        static gsize g_define_type__static = 0; \
        if (g_once_init_enter(&g_define_type__static)) { \
            static const GFlagsValue flags_values[] = { \
                __VA_ARGS__, \
                { 0, NULL, NULL }, \
            }; \
            GType g_define_type = g_flags_register_static(g_intern_static_string(#TypeName), flags_values); \
            g_once_init_leave(&g_define_type__static, g_define_type); \
        } \
        return g_define_type__static; \
    }

#ifdef G_DEFINE_ENUM_VALUE
#undef G_DEFINE_ENUM_VALUE
#endif
#define G_DEFINE_ENUM_VALUE(EnumValue, EnumNick) \
    { \
        EnumValue, #EnumValue, EnumNick \
    }

/* copied and adapted from g_warning_once (GLib 2.78) */
#if defined(G_HAVE_ISO_VARARGS) && !G_ANALYZER_ANALYZING
#define _xfw_g_message_once(...) \
    G_STMT_START { \
        static int G_PASTE(__XfwGMessageOnceBoolean, __LINE__) = 0; /* (atomic) */ \
        if (g_atomic_int_compare_and_exchange(&G_PASTE(__XfwGMessageOnceBoolean, __LINE__), \
                                              0, 1)) \
            g_message(__VA_ARGS__); \
    } \
    G_STMT_END
#elif defined(G_HAVE_GNUC_VARARGS) && !G_ANALYZER_ANALYZING
#define _xfw_g_message_once(format...) \
    G_STMT_START { \
        static int G_PASTE(__XfwGMessageOnceBoolean, __LINE__) = 0; /* (atomic) */ \
        if (g_atomic_int_compare_and_exchange(&G_PASTE(__XfwGMessageOnceBoolean, __LINE__), \
                                              0, 1)) \
            g_message(format); \
    } \
    G_STMT_END
#else
#define _xfw_g_message_once g_message
#endif

G_BEGIN_DECLS

typedef GdkPixbuf *(*XfwGetIconFunc)(GObject *wnck_object);

enum {
    WORKSPACE_MANAGER_PROP_SCREEN = 0x2000,
};

enum {
    WORKSPACE_GROUP_PROP_SCREEN = 0x3000,
    WORKSPACE_GROUP_PROP_WORKSPACE_MANAGER,
    WORKSPACE_GROUP_PROP_CAPABILITIES,
    WORKSPACE_GROUP_PROP_WORKSPACES,
    WORKSPACE_GROUP_PROP_ACTIVE_WORKSPACE,
    WORKSPACE_GROUP_PROP_MONITORS,
};

enum {
    WORKSPACE_PROP_GROUP = 0x4000,
    WORKSPACE_PROP_ID,
    WORKSPACE_PROP_NAME,
    WORKSPACE_PROP_CAPABILITIES,
    WORKSPACE_PROP_STATE,
    WORKSPACE_PROP_NUMBER,
    WORKSPACE_PROP_LAYOUT_ROW,
    WORKSPACE_PROP_LAYOUT_COLUMN,
    WORKSPACE_PROP_GEOMETRY,
};

void _libxfce4windowing_init(void);

GDesktopAppInfo *_xfw_g_desktop_app_info_get(const gchar *app_id);
GdkPixbuf *_xfw_gicon_load(GIcon *gicon, gint size, gint scale);
GIcon *_xfw_g_icon_new(const gchar *icon_name);

void _xfw_workspace_manager_install_properties(GObjectClass *gklass);
void _xfw_workspace_group_install_properties(GObjectClass *gklass);
void _xfw_workspace_install_properties(GObjectClass *gklass);
void _xfw_application_instance_free(gpointer data);

#ifdef ENABLE_X11
Window _xfw_wnck_object_get_x11_window(GObject *wnck_object);
GIcon *_xfw_wnck_object_get_gicon(GObject *wnck_object,
                                  const gchar *primary_icon_name,
                                  const gchar *secondary_icon_name,
                                  const gchar *fallback_icon_name);
#endif

G_END_DECLS

#endif /* __LIBXFCE4WINDOWING_PRIVATE_H__ */
