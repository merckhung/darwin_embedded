#include <gtk/gtk.h>
#include <stdio.h>

#include "libxfce4windowing/libxfce4windowing.h"

static gint64 start_time = 0;

#ifdef __GNUC__
#define PRINT(fmt, ...) \
    G_STMT_START { \
        gint64 _now = g_get_monotonic_time(); \
        g_print("[%3" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT "] " fmt, (_now - start_time) / 1000000, (_now - start_time) % 100000 __VA_OPT__(, ) __VA_ARGS__); \
    } \
    G_STMT_END
#else
#define PRINT(...) g_print(__VA_ARGS__)
#endif

static gboolean
reenable_output(gpointer data) {
    const gchar *output_name = data;

    PRINT("Enabling output '%s'\n", output_name);

    gchar *cmdline = g_strdup_printf("xrandr --output '%s' --auto", output_name);
    GError *error = NULL;
    if (!g_spawn_command_line_async(cmdline, &error)) {
        g_error("Failed to enable output: %s", error->message);
    }
    g_free(cmdline);

    return FALSE;
}

static gboolean
disable_output(gpointer data) {
    const gchar *output_name = data;

    PRINT("Disabling output '%s'\n", output_name);

    gchar *cmdline = g_strdup_printf("xrandr --output '%s' --off", output_name);
    GError *error = NULL;
    if (!g_spawn_command_line_async(cmdline, &error)) {
        g_error("Failed to disable output: %s", error->message);
    }
    g_free(cmdline);

    g_timeout_add_seconds(2, reenable_output, (gpointer)output_name);

    return FALSE;
}

static void
print_monitor_workarea(XfwMonitor *monitor) {
    GdkRectangle wa;
    xfw_monitor_get_workarea(monitor, &wa);
    g_print("'%s' workarea: %dx%d+%d+%d\n",
            xfw_monitor_get_connector(monitor),
            wa.width, wa.height, wa.x, wa.y);
}

static void
workarea_changed(XfwMonitor *monitor) {
    PRINT("workarea changed: ");
    print_monitor_workarea(monitor);
}

static void
screen_monitor_added(XfwScreen *screen, XfwMonitor *monitor) {
    PRINT("monitor added: ");
    print_monitor_workarea(monitor);
    g_signal_connect(monitor, "notify::workarea",
                     G_CALLBACK(workarea_changed), NULL);
}

static void
screen_monitor_removed(XfwScreen *screen, XfwMonitor *monitor) {
    PRINT("monior removed: '%s'", xfw_monitor_get_connector(monitor));
}

int
main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s OUTPUT_NAME\n", argv[0]);
        return 1;
    }

    start_time = g_get_monotonic_time();

    gtk_init(&argc, &argv);

    XfwScreen *screen = xfw_screen_get_default();
    g_signal_connect(screen, "monitor-added",
                     G_CALLBACK(screen_monitor_added), NULL);
    g_signal_connect(screen, "monitor-removed",
                     G_CALLBACK(screen_monitor_removed), NULL);

    GList *monitors = xfw_screen_get_monitors(screen);
    PRINT("at startup:\n");
    for (GList *l = monitors; l != NULL; l = l->next) {
        screen_monitor_added(screen, XFW_MONITOR(l->data));
    }
    PRINT("end at startup\n");

    g_timeout_add_seconds(1, disable_output, argv[1]);

    gtk_main();

    return 0;
}
