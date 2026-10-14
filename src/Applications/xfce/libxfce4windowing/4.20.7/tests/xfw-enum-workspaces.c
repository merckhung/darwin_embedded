#include <gtk/gtk.h>

#include "libxfce4windowing/libxfce4windowing.h"

static void
print_group(XfwWorkspaceGroup *group) {
    gint count = xfw_workspace_group_get_workspace_count(group);
    GList *monitors = xfw_workspace_group_get_monitors(group);
    XfwWorkspaceGroupCapabilities caps = xfw_workspace_group_get_capabilities(group);

    g_print("Workspace group [%p] has %d workspaces, caps=0x%x\n", group, count, caps);
    g_print("  Monitors in group:\n");
    for (GList *m = monitors; m != NULL; m = m->next) {
        XfwMonitor *monitor = XFW_MONITOR(m->data);
        g_print("    %s\n", xfw_monitor_get_description(monitor));
    }
}

static void
print_workspace(XfwWorkspace *workspace) {
    GdkRectangle *geom = xfw_workspace_get_geometry(workspace);
    g_print(
        "  [%s] %s (state=0x%d) (caps=0x%x) (pos=(%d,%d)) (geom=%dx%d+%d+%d) (group=%p)\n",
        xfw_workspace_get_id(workspace),
        xfw_workspace_get_name(workspace),
        xfw_workspace_get_state(workspace),
        xfw_workspace_get_capabilities(workspace),
        xfw_workspace_get_layout_column(workspace),
        xfw_workspace_get_layout_row(workspace),
        geom->width,
        geom->height,
        geom->x,
        geom->y,
        xfw_workspace_get_workspace_group(workspace));
}

static void
group_created(XfwWorkspaceManager *manager, XfwWorkspaceGroup *group) {
    g_print("New workspace group:\n");
    print_group(group);
}

static void
workspace_changed(XfwWorkspace *workspace) {
    g_print("Workspace changed:\n");
    print_workspace(workspace);
}

static void
connect_workspace_signals(XfwWorkspace *workspace) {
    g_signal_connect(workspace, "name-changed", G_CALLBACK(workspace_changed), NULL);
    g_signal_connect(workspace, "capabilities-changed", G_CALLBACK(workspace_changed), NULL);
    g_signal_connect(workspace, "state-changed", G_CALLBACK(workspace_changed), NULL);
    g_signal_connect(workspace, "group-changed", G_CALLBACK(workspace_changed), NULL);
    g_signal_connect(workspace, "notify::layout-col", G_CALLBACK(workspace_changed), NULL);
    g_signal_connect(workspace, "notify::layout-row", G_CALLBACK(workspace_changed), NULL);
    g_signal_connect(workspace, "notify::geometry", G_CALLBACK(workspace_changed), NULL);
}

static void
workspace_created(XfwWorkspaceManager *manager, XfwWorkspace *workspace) {
    g_print("New workspace:\n");
    print_workspace(workspace);
    connect_workspace_signals(workspace);
}

int
main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    XfwScreen *screen = xfw_screen_get_default();
    XfwWorkspaceManager *manager = xfw_screen_get_workspace_manager(screen);
    g_signal_connect(manager, "workspace-group-created", G_CALLBACK(group_created), NULL);
    g_signal_connect(manager, "workspace-created", G_CALLBACK(workspace_created), NULL);

    GList *groups = xfw_workspace_manager_list_workspace_groups(manager);
    for (GList *l = groups; l != NULL; l = l->next) {
        XfwWorkspaceGroup *group = XFW_WORKSPACE_GROUP(l->data);
        print_group(group);
    }

    GList *workspaces = xfw_workspace_manager_list_workspaces(manager);
    g_print("Workspaces on startup:\n");
    for (GList *w = workspaces; w != NULL; w = w->next) {
        XfwWorkspace *workspace = XFW_WORKSPACE(w->data);
        print_workspace(workspace);
        connect_workspace_signals(workspace);
    }
    g_print("(end of workspaces on startup)\n");

    gtk_main();

    return 0;
}
