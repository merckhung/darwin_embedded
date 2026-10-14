/* Real GIO operations under the native Darwin runtime. Test peers and data
 * are confined to this fixture, and never implement production APIs. */
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gio.h>
#include <gio/gsettingsbackend.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixconnection.h>
#include <glib/gstdio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

static const char *nonce;
static void fail(const char *operation, GError *error)
{
    fprintf(stderr, "Native GIO failure: %s: %s (errno %d)\n", operation,
            error ? error->message : "check failed", errno);
    exit(1);
}
#define CHECK(condition, operation) do { if (!(condition)) fail(operation, NULL); } while (0)
#define CALL(condition, operation) do { if (!(condition)) fail(operation, error); } while (0)

struct event_wait { GMainLoop *loop; gboolean seen; };
static gboolean timed_out(gpointer data)
{
    g_main_loop_quit(((struct event_wait *)data)->loop);
    return G_SOURCE_REMOVE;
}
static void file_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                         GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)file; (void)other;
    if (event == G_FILE_MONITOR_EVENT_CHANGED) {
        struct event_wait *wait = data;
        wait->seen = TRUE;
        g_main_loop_quit(wait->loop);
    }
}

static void files_and_monitor(void)
{
    GError *error = NULL;
    GFile *file = g_file_new_for_path("/tmp/native-gio-file");
    CALL(g_file_replace_contents(file, nonce, strlen(nonce), NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, &error), "HFS replace");
    GFileInfo *info = g_file_query_info(file, "standard::size,unix::uid,unix::gid,owner::group",
                                      G_FILE_QUERY_INFO_NONE, NULL, &error);
    CALL(info, "HFS metadata");
    CHECK(g_file_info_get_size(info) == (goffset)strlen(nonce) &&
          g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID) == getuid(), "real HFS size/UID");
    CHECK(!g_strcmp0(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP), "wheel"), "file-backed group name");
    g_object_unref(info);
    struct event_wait wait = {g_main_loop_new(NULL, FALSE), FALSE};
    GFileMonitor *monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
    CALL(monitor, "native kqueue monitor");
    CHECK(strstr(G_OBJECT_TYPE_NAME(monitor), "Kqueue") != NULL, "selected kqueue backend");
    g_signal_connect(monitor, "changed", G_CALLBACK(file_changed), &wait);
    GFileOutputStream *stream = g_file_append_to(file, G_FILE_CREATE_NONE, NULL, &error);
    CALL(stream, "HFS append stream");
    gsize amount = 0;
    CALL(g_output_stream_write_all(G_OUTPUT_STREAM(stream), "!", 1, &amount, NULL, &error) && amount == 1, "HFS stream write");
    CALL(g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, &error), "HFS stream close");
    g_object_unref(stream);
    GSource *timeout = g_timeout_source_new_seconds(5);
    g_source_set_callback(timeout, timed_out, &wait, NULL);
    g_source_attach(timeout, NULL);
    g_main_loop_run(wait.loop);
    g_source_destroy(timeout);
    g_source_unref(timeout);
    CHECK(wait.seen, "actual NOTE_WRITE delivery");
    CHECK(g_file_monitor_cancel(monitor), "kqueue monitor cancel");
    g_object_unref(monitor);
    g_main_loop_unref(wait.loop);
    int descriptor = open("/tmp/native-gio-file", O_RDONLY);
    CHECK(descriptor >= 0, "native mapped-file descriptor");
    size_t mapped_length = strlen(nonce) + 1;
    errno = 0;
    CHECK(mmap(NULL, mapped_length, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0) == MAP_FAILED &&
          errno == EACCES, "read-only fd rejects shared writable mapping");
    char *mapping = mmap(NULL, mapped_length, PROT_READ | PROT_WRITE, MAP_PRIVATE, descriptor, 0);
    CHECK(mapping != MAP_FAILED && !memcmp(mapping, nonce, strlen(nonce)) && mapping[mapped_length - 1] == '!',
          "actual HFS pager bytes");
    CHECK(close(descriptor) == 0 && mapping[mapped_length - 1] == '!', "mapped vnode retained after close");
    mapping[0] ^= 1;
    CHECK(munmap(mapping, mapped_length) == 0, "native private mapping unmap");
    gchar *contents = NULL;
    gsize length = 0;
    CALL(g_file_load_contents(file, NULL, &contents, &length, NULL, &error), "HFS read back");
    CHECK(length == strlen(nonce) + 1 && !memcmp(contents, nonce, strlen(nonce)) && contents[length - 1] == '!', "actual HFS bytes and private COW isolation");
    g_free(contents);
    GCancellable *cancel = g_cancellable_new();
    g_cancellable_cancel(cancel);
    CHECK(!g_file_load_contents(file, cancel, &contents, &length, NULL, &error) &&
          g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED), "real cancellation error");
    g_clear_error(&error);
    g_object_unref(cancel);
    CALL(g_file_delete(file, NULL, &error), "HFS delete");
    g_object_unref(file);
    struct group group, *result;
    char buffer[512];
    errno = EDOM;
    CHECK(getgrgid_r(0, &group, buffer + 1, sizeof(buffer) - 1, &result) == 0 &&
          result == &group && group.gr_gid == 0 && !strcmp(group.gr_name, "wheel") && errno == EDOM,
          "native group lookup in unaligned caller buffer");
    CHECK(getgrgid_r(0, &group, buffer, 1, &result) == ERANGE && !result, "native group ERANGE");
    CHECK(getgrgid_r(123456, &group, buffer, sizeof(buffer), &result) == 0 && !result, "missing group has no fallback");
    puts("GIO_FILES_KQUEUE_OK");
}

struct task_wait { struct event_wait event; GThread *caller; };
static void task_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
    (void)object; (void)cancel;
    struct task_wait *wait = data;
    CHECK(g_thread_self() != wait->caller, "GTask actual worker thread");
    g_task_return_int(task, 73);
}
static void task_done(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    struct task_wait *wait = data;
    GError *error = NULL;
    CHECK(g_task_propagate_int(G_TASK(result), &error) == 73 && !error && g_thread_self() == wait->caller,
          "GTask result and context delivery");
    wait->event.seen = TRUE;
    g_main_loop_quit(wait->event.loop);
}
static void tasks_and_fds(void)
{
    struct task_wait wait = {{g_main_loop_new(NULL, FALSE), FALSE}, g_thread_self()};
    GTask *task = g_task_new(NULL, NULL, task_done, &wait);
    g_task_set_task_data(task, &wait, NULL);
    g_task_run_in_thread(task, task_worker);
    g_main_loop_run(wait.event.loop);
    CHECK(wait.event.seen, "GTask completion");
    g_object_unref(task);
    g_main_loop_unref(wait.event.loop);
    int pair[2], pipefd[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && pipe(pipefd) == 0, "native socketpair/pipe");
    GError *error = NULL;
    GSocket *sockets[2];
    GSocketConnection *connections[2];
    for (unsigned i = 0; i < 2; ++i) {
        sockets[i] = g_socket_new_from_fd(pair[i], &error);
        CALL(sockets[i], "native Unix socket");
        connections[i] = g_socket_connection_factory_create_connection(sockets[i]);
        CHECK(G_IS_UNIX_CONNECTION(connections[i]), "native Unix connection type");
    }
    CALL(g_unix_connection_send_fd(G_UNIX_CONNECTION(connections[0]), pipefd[0], NULL, &error), "SCM_RIGHTS send");
    int received = g_unix_connection_receive_fd(G_UNIX_CONNECTION(connections[1]), NULL, &error);
    CALL(received >= 0, "SCM_RIGHTS receive");
    CHECK(close(pipefd[0]) == 0 && write(pipefd[1], nonce, strlen(nonce)) == (ssize_t)strlen(nonce), "original descriptor close/write");
    char bytes[64];
    CHECK(read(received, bytes, sizeof(bytes)) == (ssize_t)strlen(nonce) && !memcmp(bytes, nonce, strlen(nonce)), "received actual descriptor bytes");
    close(received); close(pipefd[1]);
    GCredentials *credentials = g_socket_get_credentials(sockets[1], &error);
    CALL(credentials, "Darwin peer credentials");
    CHECK(g_credentials_get_unix_user(credentials, &error) == getuid() && !error, "actual peer UID");
    g_object_unref(credentials);
    for (unsigned i = 0; i < 2; ++i) {
        CALL(g_io_stream_close(G_IO_STREAM(connections[i]), NULL, &error), "Unix connection close");
        g_object_unref(connections[i]); g_object_unref(sockets[i]);
    }
    puts("GIO_TASK_FD_CREDENTIALS_OK");
}

static void resources_settings_subprocess(void)
{
    GError *error = NULL;
    GResource *resource = g_resource_load("/usr/share/native.gresource", &error);
    CALL(resource, "actual gvdb resource");
    GBytes *bytes = g_resource_lookup_data(resource, "/native/payload.txt", G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    CALL(bytes, "actual compressed resource");
    gsize length = 0;
    const char *data = g_bytes_get_data(bytes, &length);
    const char expected[] = "Native GIO compressed resource: \342\202\254\n";
    CHECK(length == sizeof(expected) - 1 && !memcmp(data, expected, length), "zlib resource UTF-8 bytes");
    CHECK(!g_resource_lookup_data(resource, "/native/missing", G_RESOURCE_LOOKUP_FLAGS_NONE, &error) &&
          g_error_matches(error, G_RESOURCE_ERROR, G_RESOURCE_ERROR_NOT_FOUND), "missing resource error");
    g_clear_error(&error); g_bytes_unref(bytes); g_resource_unref(resource);
    GSettingsSchemaSource *source = g_settings_schema_source_new_from_directory("/usr/share/glib-2.0/schemas", NULL, FALSE, &error);
    CALL(source, "actual compiled settings schema");
    GSettingsSchema *schema = g_settings_schema_source_lookup(source, "org.native.Probe", FALSE);
    CHECK(schema != NULL, "settings schema lookup");
    GSettingsBackend *backend = g_keyfile_settings_backend_new("/tmp/native-gio-settings", "/", NULL);
    GSettings *settings = g_settings_new_full(schema, backend, NULL);
    CHECK(g_settings_get_uint(settings, "counter") == 5, "schema default");
    CHECK(g_settings_set_uint(settings, "counter", 42), "real keyfile settings write");
    g_settings_sync();
    g_object_unref(settings); g_object_unref(backend);
    backend = g_keyfile_settings_backend_new("/tmp/native-gio-settings", "/", NULL);
    settings = g_settings_new_full(schema, backend, NULL);
    CHECK(g_settings_get_uint(settings, "counter") == 42, "persistent keyfile settings read");
    g_object_unref(settings); g_object_unref(backend);
    g_settings_schema_unref(schema); g_settings_schema_source_unref(source);
    CHECK(g_unlink("/tmp/native-gio-settings") == 0, "settings file remove");
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                         &error, "/usr/libexec/gio-launch-desktop", "/bin/sh", "-c",
                                         "test \"$GIO_LAUNCHED_DESKTOP_FILE_PID\" = \"$$\" || exit 91; printf '%s' \"$1\"; exit 17",
                                         "native-gio-child", nonce, NULL);
    CALL(child, "Mach-O desktop helper subprocess");
    gchar *out = NULL, *err = NULL;
    CALL(g_subprocess_communicate_utf8(child, NULL, NULL, &out, &err, &error), "native subprocess pipes/wait");
    CHECK(out && !strcmp(out, nonce) && err && !*err && g_subprocess_get_if_exited(child) &&
          g_subprocess_get_exit_status(child) == 17, "actual child output/PID/exit status");
    g_free(out); g_free(err); g_object_unref(child);
    puts("GIO_RESOURCES_SETTINGS_SUBPROCESS_OK");
}

#include "test_gio_ipc.c"
#include "test_gio_dns.c"

int main(int argc, char **argv)
{
    CHECK(argc == 2 && strlen(argv[1]) == 48, "fresh nonce argument");
    nonce = argv[1];
    struct utsname actual;
    CHECK(uname(&actual) == 0, "actual uname");
    files_and_monitor();
    tasks_and_fds();
    resources_settings_subprocess();
    dbus_ipc();
    dns_records();
    printf("NATIVE_GIO_OK:%s:%s:%s\n", nonce, actual.release, actual.machine);
    return 0;
}
