#include <gtk/gtk.h>

#include "libxfce4windowing/libxfce4windowing.h"

static const struct {
    XfwWindowState state_bit;
    const gchar *name;
} state_strings[] = {
    { XFW_WINDOW_STATE_ACTIVE, "active" },
    { XFW_WINDOW_STATE_MINIMIZED, "minimized" },
    { XFW_WINDOW_STATE_MAXIMIZED, "maximized" },
    { XFW_WINDOW_STATE_FULLSCREEN, "fullscreen" },
    { XFW_WINDOW_STATE_SKIP_PAGER, "skip_pager" },
    { XFW_WINDOW_STATE_SKIP_TASKLIST, "skip_tasklist" },
    { XFW_WINDOW_STATE_PINNED, "pinned" },
    { XFW_WINDOW_STATE_SHADED, "shaded" },
    { XFW_WINDOW_STATE_ABOVE, "above" },
    { XFW_WINDOW_STATE_BELOW, "below" },
    { XFW_WINDOW_STATE_URGENT, "urgent" },
};

static const struct {
    XfwWindowCapabilities state_bit;
    const gchar *name;
} capability_strings[] = {
    { XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE, "minimize" },
    { XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE, "unminimize" },
    { XFW_WINDOW_CAPABILITIES_CAN_MAXIMIZE, "maximize" },
    { XFW_WINDOW_CAPABILITIES_CAN_UNMAXIMIZE, "unmaximize" },
    { XFW_WINDOW_CAPABILITIES_CAN_FULLSCREEN, "fullscreen" },
    { XFW_WINDOW_CAPABILITIES_CAN_UNFULLSCREEN, "unfullscreen" },
    { XFW_WINDOW_CAPABILITIES_CAN_SHADE, "shade" },
    { XFW_WINDOW_CAPABILITIES_CAN_UNSHADE, "unshade" },
    { XFW_WINDOW_CAPABILITIES_CAN_MOVE, "move" },
    { XFW_WINDOW_CAPABILITIES_CAN_RESIZE, "resize" },
    { XFW_WINDOW_CAPABILITIES_CAN_PLACE_ABOVE, "place_above" },
    { XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_ABOVE, "unplace_above" },
    { XFW_WINDOW_CAPABILITIES_CAN_PLACE_BELOW, "place_below" },
    { XFW_WINDOW_CAPABILITIES_CAN_UNPLACE_BELOW, "unplace_below" },
    { XFW_WINDOW_CAPABILITIES_CAN_CHANGE_WORKSPACE, "change_workspace" },
};

static void
print_window_state(XfwWindow *window) {
    XfwWindowState state = xfw_window_get_state(window);
    g_print("    State: [ ");
    for (gsize i = 0; i < G_N_ELEMENTS(state_strings); ++i) {
        if ((state & state_strings[i].state_bit) != 0) {
            g_print("%s ", state_strings[i].name);
        }
    }
    g_print("]\n");
}

static void
print_window_capabilities(XfwWindow *window) {
    XfwWindowCapabilities caps = xfw_window_get_capabilities(window);
    g_print("    Capabilities: [ ");
    for (gsize i = 0; i < G_N_ELEMENTS(capability_strings); ++i) {
        if ((caps & capability_strings[i].state_bit) != 0) {
            g_print("%s ", capability_strings[i].name);
        }
    }
    g_print("]\n");
}

static void
print_window_monitors(XfwWindow *window) {
    g_print("    Monitors: [");
    GList *monitors = xfw_window_get_monitors(window);
    for (GList *lm = monitors; lm != NULL; lm = lm->next) {
        XfwMonitor *monitor = XFW_MONITOR(lm->data);
        if (lm != monitors) {
            g_print(", ");
        }
        g_print("%s", xfw_monitor_get_connector(monitor));
    }
    g_print("]\n");
}

static void
print_window_workspace(XfwWindow *window) {
    XfwWorkspace *workspace = xfw_window_get_workspace(window);
    if (workspace != NULL) {
        g_print("    Workspace: [%u] %s \n", xfw_workspace_get_number(workspace), xfw_workspace_get_name(workspace));
    } else {
        g_print("    Workspace: (none)\n");
    }
}

static void
window_state_changed(XfwWindow *window) {
    g_print("Window '%s' state changed:\n", xfw_window_get_name(window));
    print_window_state(window);
}

static void
window_caps_changed(XfwWindow *window) {
    g_print("Window '%s' capabilities changed:\n", xfw_window_get_name(window));
    print_window_capabilities(window);
}

static void
window_monitors_changed(XfwWindow *window) {
    g_print("Window '%s' monitors changed:\n", xfw_window_get_name(window));
    print_window_monitors(window);
}

static void
window_workspace_changed(XfwWindow *window) {
    g_print("Window '%s' workspace changed:\n", xfw_window_get_name(window));
    print_window_workspace(window);
}

static void
window_closed(XfwWindow *window) {
    g_print("Window closed: '%s'\n", xfw_window_get_name(window));
    g_signal_handlers_disconnect_by_data(window, NULL);
}

static void
window_opened(XfwScreen *screen, XfwWindow *window) {
    (void)screen;

    XfwApplication *app = xfw_window_get_application(window);
    g_print("New window: [%s] %s\n", xfw_application_get_class_id(app), xfw_window_get_name(window));
    print_window_state(window);
    print_window_capabilities(window);
    print_window_monitors(window);
    print_window_workspace(window);

    g_signal_connect(window, "notify::state", G_CALLBACK(window_state_changed), NULL);
    g_signal_connect(window, "notify::capabilities", G_CALLBACK(window_caps_changed), NULL);
    g_signal_connect(window, "notify::monitors", G_CALLBACK(window_monitors_changed), NULL);
    g_signal_connect(window, "notify::workspace", G_CALLBACK(window_workspace_changed), NULL);
    g_signal_connect(window, "closed", G_CALLBACK(window_closed), NULL);
}

int
main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    XfwScreen *screen = xfw_screen_get_default();
    g_signal_connect(screen, "window-opened", G_CALLBACK(window_opened), NULL);

    g_print("Windows on startup:\n");
    GList *windows = xfw_screen_get_windows(screen);
    for (GList *lw = windows; lw != NULL; lw = lw->next) {
        XfwWindow *window = XFW_WINDOW(lw->data);
        window_opened(screen, window);
    }
    g_print("(end of windows on startup)\n");

    gtk_main();

    return 0;
}
