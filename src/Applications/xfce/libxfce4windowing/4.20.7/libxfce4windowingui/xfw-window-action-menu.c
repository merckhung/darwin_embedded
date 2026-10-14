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

/**
 * SECTION:xfw-window-action-menu
 * @title: XfwWindowActionMenu
 * @short_description: a #GtkMenu subclass that lists window actions
 * @stability: Unstable
 * @include: libxfce4windowingui/libxfce4windowingui.h
 *
 * #XfwWindowActionMenu is a #GtkMenu that contains actions that can be
 * performed on a toplevel window, such as minimizing, maximizing, pinning,
 * and moving to another workspace.
 **/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib/gi18n-lib.h>

#include "xfw-window-action-menu.h"
#include "libxfce4windowingui-visibility.h"

enum {
    PROP_0,
    PROP_WINDOW,
};

struct _XfwWindowActionMenu {
    GtkMenu parent;

    XfwWindow *window;

    GtkWidget *min_item;
    GtkWidget *max_item;
    GtkWidget *move_item;
    GtkWidget *resize_item;
    GtkWidget *above_item;
    GtkWidget *pin_item;
    GtkWidget *unpin_item;
    GtkWidget *move_left_item;
    GtkWidget *move_right_item;
    GtkWidget *move_up_item;
    GtkWidget *move_down_item;
    GtkWidget *move_ws_item;
    GtkWidget *move_ws_submenu;
    GtkWidget *close_item;
};

typedef struct {
    XfwWindow *window;
    union {
        XfwDirection direction;
        XfwWorkspace *new_workspace;
    } to;
} XfwWindowWorkspaceMoveData;

static const gchar *minimize_icon_names[] = {
    "window-minimize-symbolic",
    "window-minimize-symbolic.symbolic",
    "window-minimize",
    "xfce-wm-minimize",
    NULL,
};
static const gchar *maximize_icon_names[] = {
    "window-maximize-symbolic",
    "window-maximize-symbolic.symbolic",
    "window-maximize",
    "xfce-wm-maximize",
    NULL,
};
static const gchar *close_icon_names[] = {
    "window-close-symbolic",
    "window-close-symbolic.symbolic",
    "window-close",
    NULL,
};

static void xfw_window_action_menu_constructed(GObject *obj);
static void xfw_window_action_menu_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfw_window_action_menu_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfw_window_action_menu_dispose(GObject *obj);

static void toggle_minimize_state(GtkWidget *item, XfwWindow *window);
static void toggle_maximize_state(GtkWidget *item, XfwWindow *window);
static void move_window(GtkWidget *item, XfwWindow *window);
static void resize_window(GtkWidget *item, XfwWindow *window);
static void toggle_above_state(GtkWidget *item, XfwWindow *window);
static void toggle_pinned_state(GtkWidget *item, XfwWindow *window);
static void move_window_workspace(GtkWidget *item, XfwWindowWorkspaceMoveData *data);
static void close_window(GtkWidget *item, XfwWindow *window);

static void free_move_data(gpointer data, GClosure *closure);

static void update_menu_items(XfwWindowActionMenu *menu);
static void update_move_submenu(XfwWindowActionMenu *menu);

static void window_state_changed(XfwWindow *window, XfwWindowState changed_mask, XfwWindowState new_state, XfwWindowActionMenu *menu);
static void window_capabilities_changed(XfwWindow *window, XfwWindowCapabilities changed_mask, XfwWindowCapabilities new_capabilities, XfwWindowActionMenu *menu);
static void window_workspace_changed(XfwWindow *window, XfwWindowActionMenu *menu);


G_DEFINE_FINAL_TYPE(XfwWindowActionMenu, xfw_window_action_menu, GTK_TYPE_MENU)


static void
xfw_window_action_menu_class_init(XfwWindowActionMenuClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);

    gklass->constructed = xfw_window_action_menu_constructed;
    gklass->set_property = xfw_window_action_menu_set_property;
    gklass->get_property = xfw_window_action_menu_get_property;
    gklass->dispose = xfw_window_action_menu_dispose;

    /**
     * XfwWindowActionMenu:window:
     *
     * The #XfwWindow instance used to create the action menu.
     **/
    g_object_class_install_property(gklass,
                                    PROP_WINDOW,
                                    g_param_spec_object("window",
                                                        "window",
                                                        "window",
                                                        XFW_TYPE_WINDOW,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
xfw_window_action_menu_init(XfwWindowActionMenu *menu) {
}

static void
update_menu_item_image(GtkWidget *item,
                       GParamSpec *pspec,
                       const gchar **icon_names) {
    GtkIconTheme *itheme = gtk_icon_theme_get_default();
    gint scale_factor = gtk_widget_get_scale_factor(item);
    gint icon_width, icon_height, icon_size;
    GtkIconInfo *icon_info;

    gtk_icon_size_lookup(GTK_ICON_SIZE_MENU, &icon_width, &icon_height);
    icon_size = MIN(icon_width, icon_height);

    icon_info = gtk_icon_theme_choose_icon_for_scale(itheme, icon_names, icon_size, scale_factor, GTK_ICON_LOOKUP_FORCE_SIZE);
    if (G_LIKELY(icon_info != NULL)) {
        GdkPixbuf *icon = gtk_icon_info_load_icon(icon_info, NULL);

        if (G_LIKELY(icon != NULL)) {
            cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(icon, scale_factor, NULL);
            GtkWidget *img = gtk_image_new_from_surface(surface);

            gtk_widget_show(img);
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), img);
            G_GNUC_END_IGNORE_DEPRECATIONS

            g_object_unref(icon);
            cairo_surface_destroy(surface);
        }

        g_object_unref(icon_info);
    }
}

static GtkWidget *
create_image_menu_item(const gchar *label_text, const gchar **icon_names) {
    GtkWidget *item;

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    item = gtk_image_menu_item_new_with_mnemonic(label_text);
    G_GNUC_END_IGNORE_DEPRECATIONS
    update_menu_item_image(item, NULL, icon_names);

    return item;
}

static void
xfw_window_action_menu_constructed(GObject *obj) {
    XfwWindowActionMenu *menu = XFW_WINDOW_ACTION_MENU(obj);
    XfwWorkspaceManager *manager = xfw_screen_get_workspace_manager(xfw_window_get_screen(menu->window));
    XfwWindow *window = menu->window;
    GtkWidget *item;
    XfwWindowWorkspaceMoveData *mdata;

    G_OBJECT_CLASS(xfw_window_action_menu_parent_class)->constructed(obj);

    menu->min_item = item = create_image_menu_item("", minimize_icon_names);
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(toggle_minimize_state), window);
    g_signal_connect(G_OBJECT(item), "notify::scale-factor",
                     G_CALLBACK(update_menu_item_image), minimize_icon_names);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->max_item = item = create_image_menu_item("", maximize_icon_names);
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(toggle_maximize_state), window);
    g_signal_connect(G_OBJECT(item), "notify::scale-factor",
                     G_CALLBACK(update_menu_item_image), maximize_icon_names);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->move_item = item = gtk_menu_item_new_with_mnemonic(_("_Move"));
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(move_window), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->resize_item = item = gtk_menu_item_new_with_mnemonic(_("_Resize"));
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(resize_window), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->above_item = item = gtk_check_menu_item_new_with_mnemonic(_("Always on _Top"));
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(toggle_above_state), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->pin_item = item = gtk_radio_menu_item_new_with_mnemonic(NULL, _("_Always on Visible Workspace"));
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(toggle_pinned_state), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->unpin_item = item = gtk_radio_menu_item_new_with_mnemonic_from_widget(GTK_RADIO_MENU_ITEM(item), _("_Only on This Workspace"));
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(toggle_pinned_state), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->move_left_item = item = gtk_menu_item_new_with_mnemonic(_("Move to Workspace _Left"));
    mdata = g_new0(XfwWindowWorkspaceMoveData, 1);
    mdata->window = menu->window;
    mdata->to.direction = XFW_DIRECTION_LEFT;
    g_signal_connect_data(G_OBJECT(item), "activate",
                          G_CALLBACK(move_window_workspace), mdata,
                          free_move_data, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->move_right_item = item = gtk_menu_item_new_with_mnemonic(_("Move to Workspace R_ight"));
    mdata = g_new0(XfwWindowWorkspaceMoveData, 1);
    mdata->window = menu->window;
    mdata->to.direction = XFW_DIRECTION_RIGHT;
    g_signal_connect_data(G_OBJECT(item), "activate",
                          G_CALLBACK(move_window_workspace), mdata,
                          free_move_data, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->move_up_item = item = gtk_menu_item_new_with_mnemonic(_("Move to Workspace _Up"));
    mdata = g_new0(XfwWindowWorkspaceMoveData, 1);
    mdata->window = menu->window;
    mdata->to.direction = XFW_DIRECTION_UP;
    g_signal_connect_data(G_OBJECT(item), "activate",
                          G_CALLBACK(move_window_workspace), mdata,
                          free_move_data, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->move_down_item = item = gtk_menu_item_new_with_mnemonic(_("Move to Workspace _Down"));
    mdata = g_new0(XfwWindowWorkspaceMoveData, 1);
    mdata->window = menu->window;
    mdata->to.direction = XFW_DIRECTION_DOWN;
    g_signal_connect_data(G_OBJECT(item), "activate",
                          G_CALLBACK(move_window_workspace), mdata,
                          free_move_data, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->move_ws_item = item = gtk_menu_item_new_with_mnemonic(_("Move to Another _Workspace"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->move_ws_submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu->move_ws_submenu);

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    menu->close_item = item = create_image_menu_item(_("_Close"), close_icon_names);
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(close_window), window);
    g_signal_connect(G_OBJECT(item), "notify::scale-factor",
                     G_CALLBACK(update_menu_item_image), close_icon_names);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all(GTK_WIDGET(menu));
    gtk_widget_hide(GTK_WIDGET(menu));

    g_signal_connect_swapped(manager, "workspace-created", G_CALLBACK(update_move_submenu), menu);
    g_signal_connect_swapped(manager, "workspace-destroyed", G_CALLBACK(update_move_submenu), menu);

    g_signal_connect(menu->window, "state-changed", G_CALLBACK(window_state_changed), menu);
    g_signal_connect(menu->window, "capabilities-changed", G_CALLBACK(window_capabilities_changed), menu);
    g_signal_connect(menu->window, "workspace-changed", G_CALLBACK(window_workspace_changed), menu);

    update_menu_items(menu);
}

static void
xfw_window_action_menu_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
    XfwWindowActionMenu *menu = XFW_WINDOW_ACTION_MENU(obj);

    switch (prop_id) {
        case PROP_WINDOW:
            menu->window = g_object_ref(g_value_get_object(value));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_window_action_menu_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
    XfwWindowActionMenu *menu = XFW_WINDOW_ACTION_MENU(obj);

    switch (prop_id) {
        case PROP_WINDOW:
            g_value_set_object(value, menu->window);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
xfw_window_action_menu_dispose(GObject *obj) {
    XfwWindowActionMenu *menu = XFW_WINDOW_ACTION_MENU(obj);

    if (menu->window != NULL) {
        XfwWorkspaceManager *manager = xfw_screen_get_workspace_manager(xfw_window_get_screen(menu->window));
        g_signal_handlers_disconnect_by_data(manager, menu);
        g_signal_handlers_disconnect_by_data(menu->window, menu);
        g_clear_object(&menu->window);
    }

    G_OBJECT_CLASS(xfw_window_action_menu_parent_class)->dispose(obj);
}

static void
toggle_minimize_state(GtkWidget *item, XfwWindow *window) {
    xfw_window_set_minimized(window, (xfw_window_get_state(window) & XFW_WINDOW_STATE_MINIMIZED) == 0, NULL);
}

static void
toggle_maximize_state(GtkWidget *item, XfwWindow *window) {
    xfw_window_set_maximized(window, (xfw_window_get_state(window) & XFW_WINDOW_STATE_MAXIMIZED) == 0, NULL);
}
static void
move_window(GtkWidget *item, XfwWindow *window) {
    xfw_window_start_move(window, NULL);
}

static void
resize_window(GtkWidget *item, XfwWindow *window) {
    xfw_window_start_resize(window, NULL);
}

static void
toggle_above_state(GtkWidget *item, XfwWindow *window) {
    xfw_window_set_above(window, (xfw_window_get_state(window) & XFW_WINDOW_STATE_ABOVE) == 0, NULL);
}

static void
toggle_pinned_state(GtkWidget *item, XfwWindow *window) {
    xfw_window_set_pinned(window, (xfw_window_get_state(window) & XFW_WINDOW_STATE_PINNED) == 0, NULL);
}

static void
move_window_workspace(GtkWidget *item, XfwWindowWorkspaceMoveData *data) {
    XfwWorkspace *new_workspace = NULL;

    if (data->to.direction != XFW_DIRECTION_UP
        && data->to.direction != XFW_DIRECTION_DOWN
        && data->to.direction != XFW_DIRECTION_LEFT
        && data->to.direction != XFW_DIRECTION_RIGHT)
    {
        new_workspace = data->to.new_workspace;
    } else {
        XfwWorkspace *workspace = xfw_window_get_workspace(data->window);
        if (workspace != NULL) {
            new_workspace = xfw_workspace_get_neighbor(workspace, data->to.direction);
        }
    }

    if (new_workspace != NULL) {
        xfw_window_set_pinned(data->window, FALSE, NULL);
        xfw_window_move_to_workspace(data->window, new_workspace, NULL);
    }
}

static void
close_window(GtkWidget *item, XfwWindow *window) {
    xfw_window_close(window, gtk_get_current_event_time(), NULL);
}

static void
free_move_data(gpointer data, GClosure *closure) {
    XfwWindowWorkspaceMoveData *mdata = data;
    if (mdata->to.direction != XFW_DIRECTION_UP
        && mdata->to.direction != XFW_DIRECTION_DOWN
        && mdata->to.direction != XFW_DIRECTION_LEFT
        && mdata->to.direction != XFW_DIRECTION_RIGHT)
    {
        g_object_unref(mdata->to.new_workspace);
    }
    g_free(mdata);
}

static void
set_item_mnemonic(GtkWidget *item, const gchar *text) {
    GtkLabel *label = GTK_LABEL(gtk_bin_get_child(GTK_BIN(item)));
    gtk_label_set_text_with_mnemonic(label, text);
    gtk_label_set_use_underline(label, TRUE);
}

static void
update_move_submenu(XfwWindowActionMenu *menu) {
    GtkWidget *submenu = menu->move_ws_submenu, *item;
    GList *children;
    XfwWorkspaceManager *manager = xfw_screen_get_workspace_manager(xfw_window_get_screen(menu->window));
    XfwWindowCapabilities caps = xfw_window_get_capabilities(menu->window);
    XfwWorkspace *workspace = xfw_window_get_workspace(menu->window);

    children = gtk_container_get_children(GTK_CONTAINER(submenu));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_container_remove(GTK_CONTAINER(submenu), GTK_WIDGET(l->data));
    }
    g_list_free(children);

    if ((caps & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0
        && g_list_length(xfw_workspace_manager_list_workspaces(manager)) > 1)
    {
        for (GList *l = xfw_workspace_manager_list_workspaces(manager);
             l != NULL;
             l = l->next)
        {
            XfwWorkspace *other_workspace = XFW_WORKSPACE(l->data);
            gchar *label;
            XfwWindowWorkspaceMoveData *mdata = g_new0(XfwWindowWorkspaceMoveData, 1);
            mdata->window = menu->window;
            mdata->to.new_workspace = g_object_ref(other_workspace);

            label = (gchar *)xfw_workspace_get_name(other_workspace);
            guint number = xfw_workspace_get_number(other_workspace);
            number += 1;
            if (label == NULL) {
                label = g_strdup_printf(number < 10
                                            ? _("Workspace _%d")
                                            : _("Workspace %d"),
                                        number);
                item = gtk_menu_item_new_with_mnemonic(label);
                g_clear_pointer(&label, g_free);
            } else if (number >= 10) {
                item = gtk_menu_item_new_with_label(label);
            } else {
                // escape mnemonics
                GString *new_label = g_string_new(label);
                g_string_replace(new_label, "_", "__", 0);

                // locate number
                gchar *regex_pattern = g_strdup_printf("(?:^|[^\\d])(%u)(?:[^\\d]|$)", number);
                GRegex *regex = g_regex_new(regex_pattern, 0 /*G_REGEX_DEFAULT*/, 0 /*G_REGEX_MATCH_DEFAULT*/, NULL);
                GMatchInfo *match_info = NULL;

                if (g_regex_match(regex, new_label->str, 0 /*G_REGEX_MATCH_DEFAULT*/, &match_info)) {
                    // insert '_' before digit
                    gint digit_pos;
                    g_match_info_fetch_pos(match_info, 1, &digit_pos, NULL);
                    g_string_insert_c(new_label, digit_pos, '_');
                } else {
                    // append " (1)"
                    g_string_append_printf(new_label, " (_%u)", number);
                }

                g_match_info_free(match_info);
                g_regex_unref(regex);
                g_free(regex_pattern);

                item = gtk_menu_item_new_with_mnemonic(new_label->str);
                g_string_free(new_label, TRUE);
            }

            if (other_workspace == workspace) {
                gtk_widget_set_sensitive(item, FALSE);
            }
            g_signal_connect_data(G_OBJECT(item), "activate",
                                  G_CALLBACK(move_window_workspace), mdata,
                                  free_move_data, 0);
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
        }

        gtk_widget_show(menu->move_ws_item);
        gtk_widget_show_all(menu->move_ws_submenu);
    } else {
        gtk_widget_hide(menu->move_ws_item);
    }
}

static void
update_menu_items(XfwWindowActionMenu *menu) {
    XfwWindowState state = xfw_window_get_state(menu->window);
    XfwWindowCapabilities caps = xfw_window_get_capabilities(menu->window);
    XfwWorkspace *workspace = xfw_window_get_workspace(menu->window);

    set_item_mnemonic(menu->min_item, (state & XFW_WINDOW_STATE_MINIMIZED) == 0 ? _("Mi_nimize") : _("Unmi_nimize"));
    gtk_widget_set_sensitive(menu->min_item,
                             ((state & XFW_WINDOW_STATE_MINIMIZED) == 0 && (caps & XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE))
                                 || ((state & XFW_WINDOW_STATE_MINIMIZED) != 0 && (caps & XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE)));

    set_item_mnemonic(menu->max_item, (state & XFW_WINDOW_STATE_MAXIMIZED) == 0 ? _("Ma_ximize") : _("Unma_ximize"));
    gtk_widget_set_sensitive(menu->max_item,
                             ((state & XFW_WINDOW_STATE_MAXIMIZED) == 0 && (caps & XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE))
                                 || ((state & XFW_WINDOW_STATE_MAXIMIZED) != 0 && (caps & XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE)));

    gtk_widget_set_sensitive(menu->move_item, (caps & XFW_WINDOW_CAPABILITIES_CAN_MOVE) != 0);
    gtk_widget_set_sensitive(menu->resize_item, (caps & XFW_WINDOW_CAPABILITIES_CAN_RESIZE) != 0);

    g_signal_handlers_block_by_func(menu->above_item, G_CALLBACK(toggle_above_state), menu->window);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu->above_item), (state & XFW_WINDOW_STATE_ABOVE) != 0);
    gtk_widget_set_sensitive(menu->above_item,
                             ((state & XFW_WINDOW_STATE_ABOVE) == 0 && (caps & XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE))
                                 || ((state & XFW_WINDOW_STATE_ABOVE) != 0 && (caps & XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE)));
    g_signal_handlers_unblock_by_func(menu->above_item, G_CALLBACK(toggle_above_state), menu->window);

    g_signal_handlers_block_by_func(menu->pin_item, G_CALLBACK(toggle_pinned_state), menu->window);
    g_signal_handlers_block_by_func(menu->unpin_item, G_CALLBACK(toggle_pinned_state), menu->window);
    if ((state & XFW_WINDOW_STATE_PINNED) != 0) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu->pin_item), TRUE);
    } else {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu->unpin_item), TRUE);
    }
    gtk_widget_set_sensitive(menu->pin_item, (caps & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0);
    gtk_widget_set_sensitive(menu->unpin_item, (caps & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0);
    g_signal_handlers_unblock_by_func(menu->pin_item, G_CALLBACK(toggle_pinned_state), menu->window);
    g_signal_handlers_unblock_by_func(menu->unpin_item, G_CALLBACK(toggle_pinned_state), menu->window);

    if ((caps & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0 && workspace != NULL && xfw_workspace_get_neighbor(workspace, XFW_DIRECTION_LEFT) != NULL) {
        gtk_widget_show(menu->move_left_item);
    } else {
        gtk_widget_hide(menu->move_left_item);
    }
    if ((caps & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0 && workspace != NULL && xfw_workspace_get_neighbor(workspace, XFW_DIRECTION_RIGHT) != NULL) {
        gtk_widget_show(menu->move_right_item);
    } else {
        gtk_widget_hide(menu->move_right_item);
    }
    if ((caps & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0 && workspace != NULL && xfw_workspace_get_neighbor(workspace, XFW_DIRECTION_UP) != NULL) {
        gtk_widget_show(menu->move_up_item);
    } else {
        gtk_widget_hide(menu->move_up_item);
    }
    if ((caps & XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE) != 0 && workspace != NULL && xfw_workspace_get_neighbor(workspace, XFW_DIRECTION_DOWN) != NULL) {
        gtk_widget_show(menu->move_down_item);
    } else {
        gtk_widget_hide(menu->move_down_item);
    }

    update_move_submenu(menu);
}

static void
window_state_changed(XfwWindow *window, XfwWindowState changed_mask, XfwWindowState new_state, XfwWindowActionMenu *menu) {
    update_menu_items(menu);
}

static void
window_capabilities_changed(XfwWindow *window, XfwWindowCapabilities changed_mask, XfwWindowCapabilities new_capabilities, XfwWindowActionMenu *menu) {
    update_menu_items(menu);
}

static void
window_workspace_changed(XfwWindow *window, XfwWindowActionMenu *menu) {
    update_menu_items(menu);
}

/**
 * xfw_window_action_menu_new: (constructor)
 * @window: (not nullable) (transfer none): an #XfwWindow.
 *
 * Creates a new window action menu that acts on @window.
 *
 * Return value: (not nullable) (transfer full): a new #XfwWindowActionMenu
 * instance, with a floating reference owned by the caller.
 **/
GtkWidget *
xfw_window_action_menu_new(XfwWindow *window) {
    return g_object_new(XFW_TYPE_WINDOW_ACTION_MENU,
                        "window", window,
                        NULL);
}

#define __XFW_WINDOW_ACTION_MENU_C__
#include "libxfce4windowingui-visibility.c"
