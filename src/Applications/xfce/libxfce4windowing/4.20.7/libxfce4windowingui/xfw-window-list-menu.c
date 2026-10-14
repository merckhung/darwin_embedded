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
 * SECTION:xfw-window-list-menu
 * @title: XfwWindowListMenu
 * @short_description: a #GtkMenu subclass that lists all windows
 * @stability: Unstable
 * @include: libxfce4windowingui/libxfce4windowingui.h
 *
 * #XfwWindowListMenu is a #GtkMenu that lists all windows present on the
 * desktop, categorized by workspace.  There are also various options that
 * control presentation; see the properties available on the object.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib-object.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>
#include <libxfce4windowing/libxfce4windowing.h>

#include "xfw-window-action-menu.h"
#include "xfw-window-list-menu.h"
#include "libxfce4windowingui-visibility.h"

#define MIN_MINIMIZED_ICON_SATURATION (0)
#define MAX_MINIMIZED_ICON_SATURATION (100)
#define DEFAULT_MINIMIZED_ICON_SATURATION (50)
#define MIN_MAX_WIDTH_CHARS (-1)
#define MAX_MAX_WIDTH_CHARS (G_MAXINT)
#define DEFAULT_MAX_WIDTH_CHARS (24)
#define DEFAULT_ELLIPSIZE_MODE (PANGO_ELLIPSIZE_MIDDLE)

struct _XfwWindowListMenu {
    GtkMenu parent;

    XfwScreen *screen;

    gboolean show_icons;
    gboolean show_workspace_names;
    gboolean show_workspace_submenus;
    gboolean show_sticky_windows_once;
    gboolean show_urgent_windows_section;
    gboolean show_workspace_actions;
    gboolean show_all_workspaces;

    gint minimized_icon_saturation;
    PangoEllipsizeMode window_title_ellipsize_mode;
    gint window_title_max_width_chars;
};

enum {
    PROP_0,
    PROP_SCREEN,
    PROP_SHOW_ICONS,
    PROP_SHOW_WORKSPACE_NAMES,
    PROP_SHOW_WORKSPACE_SUBMENUS,
    PROP_SHOW_STICKY_WINDOWS_ONCE,
    PROP_SHOW_URGENT_WINDOWS_SECTION,
    PROP_SHOW_WORKSPACE_ACTIONS,
    PROP_SHOW_ALL_WORKSPACES,
    PROP_MINIMIZED_ICON_SATURATION,
    PROP_WINDOW_TITLE_ELLIPSIZE_MODE,
    PROP_WINDOW_TITLE_MAX_WIDTH_CHARS,
};

static void xfw_window_list_menu_set_property(GObject *object,
                                              guint property_id,
                                              const GValue *value,
                                              GParamSpec *pspec);
static void xfw_window_list_menu_get_property(GObject *object,
                                              guint property_id,
                                              GValue *value,
                                              GParamSpec *pspec);
static void xfw_window_list_menu_finalize(GObject *object);

static void xfw_window_list_menu_show(GtkWidget *widget);

static void populate_window_list_menu(XfwWindowListMenu *menu);


G_DEFINE_FINAL_TYPE(XfwWindowListMenu, xfw_window_list_menu, GTK_TYPE_MENU)


static void
xfw_window_list_menu_class_init(XfwWindowListMenuClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->set_property = xfw_window_list_menu_set_property;
    gobject_class->get_property = xfw_window_list_menu_get_property;
    gobject_class->finalize = xfw_window_list_menu_finalize;

    /**
     * XfwWindowListMenu:screen:
     *
     * The #XfwScreen to use when populating the menu.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SCREEN,
                                    g_param_spec_object("screen",
                                                        "screen",
                                                        "XfwScreen",
                                                        XFW_TYPE_SCREEN,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:show-icons:
     *
     * Whether or not to show icons in the menu.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SHOW_ICONS,
                                    g_param_spec_boolean("show-icons",
                                                         "show-icons",
                                                         "Show icons in the menu",
                                                         TRUE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:show-workspace-names:
     *
     * Whether or not to show a heading with the workspace name before the list
     * of windows on that workspace.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SHOW_WORKSPACE_NAMES,
                                    g_param_spec_boolean("show-workspace-names",
                                                         "show-workspace-names",
                                                         "Show headings with each workspace name",
                                                         TRUE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:show-workspace-submenus:
     *
     * Whether or not the lists of windows should be in submenus for each
     * workspace.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SHOW_WORKSPACE_SUBMENUS,
                                    g_param_spec_boolean("show-workspace-submenus",
                                                         "show-workspace-submenus",
                                                         "Show the contents of each workspace in a submenu",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:show-sticky-windows-once:
     *
     * Whether or not sticky/pinned windows should be shown once (on the active
     * workspace), or in each workspace list.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SHOW_STICKY_WINDOWS_ONCE,
                                    g_param_spec_boolean("show-sticky-windows-once",
                                                         "show-sticky-windows-once",
                                                         "Show sticky windows once, rather than on every workspace",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:show-urgent-windows-section:
     *
     * Whether or not to show an extra section that lists urgent windows on
     * other workspaces.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SHOW_URGENT_WINDOWS_SECTION,
                                    g_param_spec_boolean("show-urgent-windows-section",
                                                         "show-urgent-windows-section",
                                                         "Add a section for windows that are asking for attention",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:show-workspace-actions:
     *
     * Whether or not to show a section in the menu with items to add and
     * remove workspaces.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SHOW_WORKSPACE_ACTIONS,
                                    g_param_spec_boolean("show-workspace-actions",
                                                         "show-workspace-actions",
                                                         "Show menu items for adding and removing workspaces",
                                                         TRUE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:show-all-workspaces:
     *
     * Whether or not to show all workspaces in the list, or just the current
     * workspace.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_SHOW_ALL_WORKSPACES,
                                    g_param_spec_boolean("show-all-workspaces",
                                                         "show-all-workspaces",
                                                         "Show windows from all workspaces, rather than just the current workspace",
                                                         TRUE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:minimized-icon-saturation:
     *
     * The saturation of icons for minimized windows.  The value should be
     * between 0 and 100.  Lower values will make the icon look more like a
     * greyscale image.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_MINIMIZED_ICON_SATURATION,
                                    g_param_spec_int("minimized-icon-saturation",
                                                     "minimized-icon-saturation",
                                                     "Saturation of minimized window icons",
                                                     MIN_MINIMIZED_ICON_SATURATION,
                                                     MAX_MINIMIZED_ICON_SATURATION,
                                                     DEFAULT_MINIMIZED_ICON_SATURATION,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:window-title-ellipsize-mode:
     *
     * The #PangoEllipsizeMode to use when ellipsizing window titles.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_WINDOW_TITLE_ELLIPSIZE_MODE,
                                    g_param_spec_enum("window-title-ellipsize-mode",
                                                      "window-title-eelipsize-mode",
                                                      "Ellipsize mode to use for the window title labels",
                                                      PANGO_TYPE_ELLIPSIZE_MODE,
                                                      DEFAULT_ELLIPSIZE_MODE,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * XfwWindowListMenu:window-title-max-width-chars:
     *
     * The maximum width (in characters) of window titles to display before
     * ellipsizing.
     **/
    g_object_class_install_property(gobject_class,
                                    PROP_WINDOW_TITLE_MAX_WIDTH_CHARS,
                                    g_param_spec_int("window-title-max-width-chars",
                                                     "window-title-max-width-chars",
                                                     "Maximum width (in characters) before window titles ellipsize",
                                                     MIN_MAX_WIDTH_CHARS,
                                                     MAX_MAX_WIDTH_CHARS,
                                                     DEFAULT_MAX_WIDTH_CHARS,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->show = xfw_window_list_menu_show;
}

static void
xfw_window_list_menu_init(XfwWindowListMenu *menu) {
    menu->show_icons = TRUE;
    menu->show_workspace_names = TRUE;
    menu->show_workspace_actions = TRUE;
    menu->show_all_workspaces = TRUE;

    menu->minimized_icon_saturation = DEFAULT_MINIMIZED_ICON_SATURATION;
    menu->window_title_ellipsize_mode = DEFAULT_ELLIPSIZE_MODE;
    menu->window_title_max_width_chars = DEFAULT_MAX_WIDTH_CHARS;

    gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
}

static void
xfw_window_list_menu_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec) {
    XfwWindowListMenu *menu = XFW_WINDOW_LIST_MENU(object);

    switch (property_id) {
        case PROP_SCREEN:
            menu->screen = g_value_dup_object(value);
            break;

        case PROP_SHOW_ICONS:
            menu->show_icons = g_value_get_boolean(value);
            break;

        case PROP_SHOW_WORKSPACE_NAMES:
            menu->show_workspace_names = g_value_get_boolean(value);
            break;

        case PROP_SHOW_WORKSPACE_SUBMENUS:
            menu->show_workspace_submenus = g_value_get_boolean(value);
            break;

        case PROP_SHOW_STICKY_WINDOWS_ONCE:
            menu->show_sticky_windows_once = g_value_get_boolean(value);
            break;

        case PROP_SHOW_URGENT_WINDOWS_SECTION:
            menu->show_urgent_windows_section = g_value_get_boolean(value);
            break;

        case PROP_SHOW_WORKSPACE_ACTIONS:
            menu->show_workspace_actions = g_value_get_boolean(value);
            break;

        case PROP_SHOW_ALL_WORKSPACES:
            menu->show_all_workspaces = g_value_get_boolean(value);
            break;

        case PROP_MINIMIZED_ICON_SATURATION:
            menu->minimized_icon_saturation = CLAMP(g_value_get_int(value),
                                                    MIN_MINIMIZED_ICON_SATURATION,
                                                    MAX_MINIMIZED_ICON_SATURATION);
            break;

        case PROP_WINDOW_TITLE_ELLIPSIZE_MODE:
            menu->window_title_ellipsize_mode = g_value_get_enum(value);
            break;

        case PROP_WINDOW_TITLE_MAX_WIDTH_CHARS:
            menu->window_title_max_width_chars = CLAMP(g_value_get_int(value),
                                                       MIN_MAX_WIDTH_CHARS,
                                                       MAX_MAX_WIDTH_CHARS);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void
xfw_window_list_menu_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec) {
    XfwWindowListMenu *menu = XFW_WINDOW_LIST_MENU(object);

    switch (property_id) {
        case PROP_SCREEN:
            g_value_set_object(value, menu->screen);
            break;

        case PROP_SHOW_ICONS:
            g_value_set_boolean(value, menu->show_icons);
            break;

        case PROP_SHOW_WORKSPACE_NAMES:
            g_value_set_boolean(value, menu->show_workspace_names);
            break;

        case PROP_SHOW_WORKSPACE_SUBMENUS:
            g_value_set_boolean(value, menu->show_workspace_submenus);
            break;

        case PROP_SHOW_STICKY_WINDOWS_ONCE:
            g_value_set_boolean(value, menu->show_sticky_windows_once);
            break;

        case PROP_SHOW_URGENT_WINDOWS_SECTION:
            g_value_set_boolean(value, menu->show_urgent_windows_section);
            break;

        case PROP_SHOW_WORKSPACE_ACTIONS:
            g_value_set_boolean(value, menu->show_workspace_actions);
            break;

        case PROP_SHOW_ALL_WORKSPACES:
            g_value_set_boolean(value, menu->show_all_workspaces);
            break;

        case PROP_MINIMIZED_ICON_SATURATION:
            g_value_set_int(value, menu->minimized_icon_saturation);
            break;

        case PROP_WINDOW_TITLE_ELLIPSIZE_MODE:
            g_value_set_enum(value, menu->window_title_ellipsize_mode);
            break;

        case PROP_WINDOW_TITLE_MAX_WIDTH_CHARS:
            g_value_set_int(value, menu->window_title_max_width_chars);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void
xfw_window_list_menu_finalize(GObject *object) {
    XfwWindowListMenu *menu = XFW_WINDOW_LIST_MENU(object);
    g_object_unref(menu->screen);
    G_OBJECT_CLASS(xfw_window_list_menu_parent_class)->finalize(object);
}

static void
xfw_window_list_menu_show(GtkWidget *widget) {
    populate_window_list_menu(XFW_WINDOW_LIST_MENU(widget));
    GTK_WIDGET_CLASS(xfw_window_list_menu_parent_class)->show(widget);
}

static void
workspace_menu_item_activate(XfwWorkspace *workspace) {
    xfw_workspace_activate(workspace, NULL);
}

static gchar *
sanitize_displayed_name(const gchar *name, const gchar *fallback) {
    if (name == NULL || name[0] == '\0') {
        return fallback != NULL ? g_markup_escape_text(fallback, -1) : NULL;
    } else if (!g_utf8_validate(name, -1, NULL)) {
        gchar *converted = g_locale_to_utf8(name, -1, NULL, NULL, NULL);
        if (converted != NULL) {
            gchar *escaped = g_markup_escape_text(converted, -1);
            g_free(converted);
            return escaped;
        } else {
            return fallback != NULL ? g_markup_escape_text(fallback, -1) : NULL;
        }
    } else {
        return g_markup_escape_text(name, -1);
    }
}

static GtkWidget *
add_workspace_header(XfwWindowListMenu *menu, XfwWorkspaceGroup *group, gint group_number, XfwWorkspace *workspace) {
    char style_tag_char;
    if (workspace == xfw_workspace_group_get_active_workspace(group)) {
        style_tag_char = 'b';
    } else {
        style_tag_char = 'i';
    }

    gchar *ws_label = sanitize_displayed_name(xfw_workspace_get_name(workspace), NULL);
    if (ws_label == NULL) {
        guint ws_num = xfw_workspace_get_number(workspace);
        if (group_number != -1) {
            ws_label = g_strdup_printf(_("Group %u, Workspace %u"), group_number + 1, ws_num + 1);
        } else {
            ws_label = g_strdup_printf(_("Workspace %u"), ws_num + 1);
        }
    }

    gchar *ws_markup = g_strdup_printf("<%c>%s</%c>", style_tag_char, ws_label, style_tag_char);

    GtkWidget *mi = gtk_menu_item_new_with_label(ws_markup);
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(mi));
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);

    gtk_widget_show(mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    g_free(ws_label);
    g_free(ws_markup);

    return mi;
}

static void
window_closed(XfwWindow *window, GtkWidget *mi) {
    GtkWidget *menu = gtk_widget_get_parent(mi);
    if (GTK_IS_CONTAINER(menu)) {
        gtk_container_remove(GTK_CONTAINER(menu), mi);
    }
}

static XfwSeat *
find_xfw_seat_for_gdk_seat(XfwScreen *screen, GdkSeat *gdk_seat) {
    g_return_val_if_fail(XFW_IS_SCREEN(screen), NULL);
    g_return_val_if_fail(gdk_seat == NULL || GDK_IS_SEAT(gdk_seat), NULL);

    if (gdk_seat == NULL) {
        gdk_seat = gdk_display_get_default_seat(gdk_display_get_default());
    }
    GdkDisplay *display = gdk_seat_get_display(gdk_seat);
    GList *gseats = gdk_display_list_seats(display);
    GList *xseats = xfw_screen_get_seats(screen);

    XfwSeat *xseat = NULL;

    if (g_list_length(gseats) == g_list_length(xseats)) {
        for (GList *gl = gseats, *xl = xseats; gl != NULL && xl != NULL; gl = gl->next, xl = xl->next) {
            if (gdk_seat == GDK_SEAT(gl->data)) {
                xseat = XFW_SEAT(xl->data);
                break;
            }
        }
    }

    g_list_free(gseats);

    return xseat;
}

static void
window_menu_item_activate(GtkWidget *mi, XfwWindow *window) {
    if (!xfw_window_is_pinned(window)) {
        xfw_workspace_activate(xfw_window_get_workspace(window), NULL);
    }

    XfwSeat *seat = NULL;
    GdkDevice *device = gtk_get_current_event_device();
    if (device != NULL) {
        XfwScreen *screen = xfw_window_get_screen(window);
        GdkSeat *gdk_seat = gdk_device_get_seat(device);
        seat = find_xfw_seat_for_gdk_seat(screen, gdk_seat);
    }

    xfw_window_activate(window, seat, gtk_get_current_event_time(), NULL);
}

static gboolean
window_menu_item_button_press(GtkWidget *mi, GdkEventButton *event, XfwWindow *window) {
    if (event->button == GDK_BUTTON_SECONDARY) {
        GtkWidget *parent_menu = gtk_widget_get_parent(mi);
        GtkWidget *action_menu = xfw_window_action_menu_new(window);

        g_signal_connect_swapped(action_menu, "deactivate",
                                 G_CALLBACK(gtk_menu_shell_deactivate), parent_menu);
        g_signal_connect_after(action_menu, "selection-done",
                               G_CALLBACK(gtk_widget_destroy), NULL);

        gtk_menu_popup_at_pointer(GTK_MENU(action_menu), (GdkEvent *)event);

        return TRUE;
    } else {
        return FALSE;
    }
}

static void
window_menu_item_destroyed(GtkWidget *mi, XfwWindow *window) {
    g_signal_handlers_disconnect_by_data(window, mi);
}

static void
add_workspace(GtkWidget *mi, XfwWorkspaceGroup *group) {
    xfw_workspace_group_create_workspace(group, NULL, NULL);
}

static void
remove_workspace(GtkWidget *mi, XfwWorkspace *workspace) {
    XfwWorkspace *current_workspace = xfw_workspace_group_get_active_workspace(xfw_workspace_get_workspace_group(workspace));
    gchar *current_workspace_name;
    gint current_workspace_num;
    if (current_workspace != NULL) {
        current_workspace_num = xfw_workspace_get_number(current_workspace);
        current_workspace_name = sanitize_displayed_name(xfw_workspace_get_name(current_workspace), NULL);
    } else {
        current_workspace_name = NULL;
        current_workspace_num = -1;
    }

    gchar *workspace_name = sanitize_displayed_name(xfw_workspace_get_name(workspace), NULL);
    gchar *primary, *secondary;
    if (workspace_name == NULL) {
        gint workspace_num = xfw_workspace_get_number(workspace);
        if (current_workspace_num > -1) {
            primary = g_strdup_printf(_("Do you really want to remove workspace %d?"), workspace_num);
            secondary = g_strdup_printf(_("You are currently on workspace %d."), current_workspace_num);
        } else {
            primary = g_strdup_printf(_("Do you really want to remove workspace %d?"), workspace_num);
            secondary = NULL;
        }
    } else {
        gchar *last_ws_name_esc = g_markup_escape_text(workspace_name, strlen(workspace_name));
        if (current_workspace_name != NULL) {
            primary = g_strdup_printf(_("Do you really want to remove workspace '%s'?"), last_ws_name_esc);
            secondary = g_strdup_printf(_("You are currently on workspace '%s'."), current_workspace_name);
        } else {
            primary = g_strdup_printf(_("Do you really want to remove workspace '%s'?"), last_ws_name_esc);
            secondary = NULL;
        }
        g_free(last_ws_name_esc);
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Remove Workspace"),
                                                    NULL,
                                                    0,
                                                    _("_Remove"),
                                                    GTK_RESPONSE_ACCEPT,
                                                    _("_Cancel"),
                                                    GTK_RESPONSE_CANCEL,
                                                    NULL);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    gchar *message = g_strdup_printf("<b>%s</b>%s%s",
                                     primary,
                                     secondary != NULL ? "\n\n" : "",
                                     secondary != NULL ? secondary : "");

    GtkWidget *label = gtk_label_new(message);
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_widget_show(label);
    gtk_container_add(GTK_CONTAINER(box), label);

    GtkResponseType response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        xfw_workspace_remove(workspace, NULL);
    }

    g_free(current_workspace_name);
    g_free(workspace_name);
    g_free(primary);
    g_free(secondary);
    g_free(message);
}

static void
add_window_menu_item(XfwWindowListMenu *menu,
                     GtkMenu *workspace_menu,
                     XfwWindow *window,
                     gint icon_width,
                     gint icon_height) {

    const gchar *leading_text, *trailing_text;
    if (xfw_window_is_active(window)) {
        leading_text = "<b><i>";
        trailing_text = "</i></b>";
    } else if (xfw_window_is_shaded(window)) {
        if (xfw_window_is_urgent(window)) {
            leading_text = "<b>=</b>";
            trailing_text = "<b>=</b>";
        } else {
            leading_text = "=";
            trailing_text = "=";
        }
    } else if (xfw_window_is_minimized(window)) {
        if (xfw_window_is_urgent(window)) {
            leading_text = "<b>[</b>";
            trailing_text = "<b>]</b>";
        } else {
            leading_text = "[";
            trailing_text = "]";
        }
    } else {
        leading_text = "";
        trailing_text = "";
    }

    gchar *title = sanitize_displayed_name(xfw_window_get_name(window), _("(Unnamed window)"));
    gchar *name = g_strconcat(leading_text, title, trailing_text, NULL);
    g_free(title);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget *mi = gtk_image_menu_item_new_with_label(name);
    G_GNUC_END_IGNORE_DEPRECATIONS

    GtkWidget *label = gtk_bin_get_child(GTK_BIN(mi));
    g_assert(GTK_IS_LABEL(label));
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), menu->window_title_ellipsize_mode);
    gtk_label_set_max_width_chars(GTK_LABEL(label), menu->window_title_max_width_chars);

    g_free(name);

    if (menu->show_icons) {
        gint icon_size = icon_width > icon_height ? icon_width : icon_height;
        gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(menu));
        GdkPixbuf *icon = xfw_window_get_icon(window, icon_size, scale_factor);
        if (icon != NULL) {
            GdkPixbuf *pixbuf_to_free = NULL;

            gint w = gdk_pixbuf_get_width(icon);
            gint h = gdk_pixbuf_get_height(icon);
            if (w != icon_width * scale_factor || h != icon_height * scale_factor) {
                GdkPixbuf *tmp = gdk_pixbuf_scale_simple(icon,
                                                         icon_width * scale_factor,
                                                         icon_height * scale_factor,
                                                         GDK_INTERP_BILINEAR);
                icon = tmp;
                pixbuf_to_free = tmp;
            }

            if (menu->minimized_icon_saturation < 100
                && (xfw_window_is_minimized(window) || xfw_window_is_shaded(window)))
            {
                if (pixbuf_to_free == NULL) {
                    icon = gdk_pixbuf_copy(icon);
                    pixbuf_to_free = icon;
                }

                /* minimized window, fade out app icon */
                gdk_pixbuf_saturate_and_pixelate(icon, icon, (gdouble)menu->minimized_icon_saturation / 100.0, TRUE);
            }

            cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(icon, scale_factor, NULL);
            GtkWidget *image = gtk_image_new_from_surface(surface);
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi), image);
            G_GNUC_END_IGNORE_DEPRECATIONS

            cairo_surface_destroy(surface);
            if (pixbuf_to_free != NULL) {
                g_object_unref(pixbuf_to_free);
            }
        }
    }

    gtk_widget_show(mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(workspace_menu), mi);

    g_signal_connect(window, "closed",
                     G_CALLBACK(window_closed), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
                     G_CALLBACK(window_menu_item_activate), window);
    g_signal_connect(G_OBJECT(mi), "button-press-event",
                     G_CALLBACK(window_menu_item_button_press), window);
    g_signal_connect(G_OBJECT(mi), "destroy",
                     G_CALLBACK(window_menu_item_destroyed), window);
}

static void
add_separator_item(GtkMenu *menu) {
    GtkWidget *mi = gtk_separator_menu_item_new();
    gtk_widget_show(mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void
populate_window_list_menu(XfwWindowListMenu *menu) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    gint icon_width, icon_height;
    gtk_icon_size_lookup(GTK_ICON_SIZE_MENU, &icon_width, &icon_height);

    XfwWorkspaceManager *workspace_manager = xfw_screen_get_workspace_manager(menu->screen);
    GList *groups = xfw_workspace_manager_list_workspace_groups(workspace_manager);

    guint group_num = 0;
    gboolean first = TRUE;

    for (GList *gl = groups; gl != NULL; gl = gl->next, ++group_num) {
        XfwWorkspaceGroup *group = XFW_WORKSPACE_GROUP(gl->data);
        XfwWorkspace *active_workspace = xfw_workspace_group_get_active_workspace(group);
        XfwWorkspace *workspace = NULL;

        GList *urgent_windows = NULL;

        for (GList *wl = xfw_workspace_group_list_workspaces(group); wl != NULL; wl = wl->next) {
            workspace = XFW_WORKSPACE(wl->data);

            gboolean show_this_workspace = menu->show_all_workspaces || workspace == active_workspace;

            GtkWidget *header_item = NULL;
            GtkWidget *submenu = NULL;
            if (show_this_workspace) {
                if (menu->show_workspace_names || menu->show_workspace_submenus) {
                    if (!first && !menu->show_workspace_submenus) {
                        add_separator_item(GTK_MENU(menu));
                        first = FALSE;
                    }
                    header_item = add_workspace_header(menu, group, group_num, workspace);
                    if (menu->show_workspace_submenus) {
                        submenu = gtk_menu_new();
                        gtk_menu_set_reserve_toggle_size(GTK_MENU(submenu), FALSE);
                        gtk_menu_item_set_submenu(GTK_MENU_ITEM(header_item), submenu);
                    } else {
                        GtkWidget *label = gtk_bin_get_child(GTK_BIN(header_item));
                        gtk_label_set_xalign(GTK_LABEL(label), 0.5f);

                        g_signal_connect_swapped(header_item, "activate",
                                                 G_CALLBACK(workspace_menu_item_activate), workspace);
                    }
                }
            }

            for (GList *l = xfw_screen_get_windows_stacked(menu->screen); l; l = l->next) {
                XfwWindow *window = XFW_WINDOW(l->data);

                if ((xfw_window_get_workspace(window) == workspace
                     || (xfw_window_is_pinned(window) && (!menu->show_sticky_windows_once || workspace == active_workspace)))
                    && !xfw_window_is_skip_pager(window)
                    && !xfw_window_is_skip_tasklist(window))
                {
                    if (show_this_workspace) {
                        if (!first && header_item == NULL) {
                            add_separator_item(GTK_MENU(menu));
                            first = FALSE;
                        }
                        add_window_menu_item(menu,
                                             submenu != NULL ? GTK_MENU(submenu) : GTK_MENU(menu),
                                             window,
                                             icon_width,
                                             icon_height);
                    }

                    if (menu->show_urgent_windows_section
                        && xfw_window_is_urgent(window)
                        && active_workspace != workspace
                        && !xfw_window_is_pinned(window))
                    {
                        urgent_windows = g_list_append(urgent_windows, window);
                    }
                }
            }
        }

        if (urgent_windows != NULL) {
            add_separator_item(GTK_MENU(menu));

            GtkWidget *urgent_item = gtk_menu_item_new_with_label(_("Urgent Windows"));
            gtk_widget_set_sensitive(urgent_item, FALSE);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), urgent_item);
            gtk_widget_show(urgent_item);

            GtkWidget *label = gtk_bin_get_child(GTK_BIN(urgent_item));
            g_assert(GTK_IS_LABEL(label));
            gtk_label_set_xalign(GTK_LABEL(label), 0.5f);

            for (GList *ul = urgent_windows; ul != NULL; ul = ul->next) {
                XfwWindow *urgent_window = XFW_WINDOW(ul->data);
                add_window_menu_item(menu, GTK_MENU(menu), urgent_window, icon_width, icon_height);
            }

            g_list_free(urgent_windows);
        }

        if (menu->show_workspace_actions) {
            gboolean can_add = (xfw_workspace_group_get_capabilities(group) & XFW_WORKSPACE_GROUP_CAPABILITIES_CREATE_WORKSPACE) != 0;
            gboolean can_remove = workspace != NULL && (xfw_workspace_get_capabilities(workspace) & XFW_WORKSPACE_CAPABILITIES_REMOVE) != 0;

            if (can_add || can_remove) {
                add_separator_item(GTK_MENU(menu));
            }

            if (can_add) {
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                GtkWidget *mi = gtk_image_menu_item_new_with_mnemonic(_("_Add Workspace"));
                G_GNUC_END_IGNORE_DEPRECATIONS
                if (menu->show_icons) {
                    GtkWidget *image = gtk_image_new_from_icon_name("list-add", GTK_ICON_SIZE_MENU);
                    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi), image);
                    G_GNUC_END_IGNORE_DEPRECATIONS
                }
                gtk_widget_show(mi);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
                g_signal_connect(G_OBJECT(mi), "activate",
                                 G_CALLBACK(add_workspace), group);
            }

            if (can_remove) {
                gint nworkspaces = xfw_workspace_group_get_workspace_count(group);

                gchar *label_text;
                gchar *ws_name = sanitize_displayed_name(xfw_workspace_get_name(workspace), NULL);
                if (ws_name == NULL) {
                    label_text = g_strdup_printf(_("_Remove Workspace %d"), nworkspaces);
                } else {
                    label_text = g_strdup_printf(_("_Remove Workspace \"%s\""), ws_name);
                }
                g_free(ws_name);

                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                GtkWidget *mi = gtk_image_menu_item_new_with_mnemonic(label_text);
                G_GNUC_END_IGNORE_DEPRECATIONS
                if (menu->show_icons) {
                    GtkWidget *image = gtk_image_new_from_icon_name("list-remove", GTK_ICON_SIZE_MENU);
                    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi), image);
                    G_GNUC_END_IGNORE_DEPRECATIONS
                }
                gtk_widget_set_sensitive(mi, nworkspaces > 1);
                gtk_widget_show(mi);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
                g_signal_connect(G_OBJECT(mi), "activate",
                                 G_CALLBACK(remove_workspace), workspace);

                g_free(label_text);
            }
        }
    }
}

/**
 * xfw_window_list_menu_new:
 * @screen: an #XfwScreen.
 *
 * Creates a new #XfwWindowListMenu, populating it from @screen.
 *
 * Return value: (not nullable) (transfer floating): A #GtkMenu subclass as a
 * #GtkWidget, with a floating reference.
 **/
GtkWidget *
xfw_window_list_menu_new(XfwScreen *screen) {
    g_return_val_if_fail(XFW_IS_SCREEN(screen), NULL);
    return g_object_new(XFW_TYPE_WINDOW_LIST_MENU,
                        "screen", screen,
                        NULL);
}

#define __XFW_WINDOW_LIST_MENU_C__
#include "libxfce4windowingui-visibility.c"
