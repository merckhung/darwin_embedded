/* Actual native account semantics, message codec and independent bus processes. */
#include <dbus/dbus.h>
#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* The SDK's getgroups names Apple's account-membership extension. The
 * original kernel entry separately observes real setgroups credentials. */
extern int native_kernel_getgroups(int, gid_t []) __asm("_getgroups");

static int fail(const char *stage, int code)
{
    printf("Native dbus failure: %s:%d\n", stage, code); fflush(stdout);
    return 1;
}

static void *group_worker(void *argument)
{
    const char *name = (uintptr_t)argument & 1 ? "staff" : "wheel";
    gid_t gid = (uintptr_t)argument & 1 ? 20 : 0;
    for (unsigned i = 0; i < 16; ++i) {
        errno = EBUSY;
        struct group *group = getgrnam(name);
        if (!group || group->gr_gid != gid || strcmp(group->gr_name, name) || errno != EBUSY) return (void *)1;
        // Force overlapping native threads to retain their group records.
        if (usleep(1000)) return (void *)2;
        struct passwd *password = getpwnam("root");
        if (!password || password->pw_uid != 0 || group->gr_gid != gid || strcmp(group->gr_name, name)) return (void *)3;
        group = getgrgid(gid);
        if (!group || strcmp(group->gr_name, name) || !group->gr_mem || !group->gr_mem[0]) return (void *)4;
        setgrent();
        group = getgrent();
        if (!group || group->gr_gid != 0 || strcmp(group->gr_name,"wheel")) return (void *)5;
        if (usleep(1000)) return (void *)6;
        group = getgrent();
        if (!group || group->gr_gid != 20 || strcmp(group->gr_name,"staff")) return (void *)7;
        if (!getgrnam(name)) return (void *)8; /* ordinary lookup does not move enumeration */
        group = getgrent();
        if (!group || group->gr_gid != 81 || strcmp(group->gr_name,"messagebus")) return (void *)9;
        endgrent();
    }
    return NULL;
}

static int accounts(void)
{
    char buffer[1024];
    struct group group, *result = NULL;
    errno = EBUSY;
    if (getgrnam_r("staff", &group, buffer + 1, sizeof(buffer) - 1, &result) || !result ||
        group.gr_gid != 20 || strcmp(group.gr_name, "staff") || !group.gr_mem[0] ||
        strcmp(group.gr_mem[0], "desktop") || group.gr_mem[1] || errno != EBUSY) return 1;
    if (getgrnam_r("staff", &group, buffer, 1, &result) != ERANGE || result) return 2;
    if (getgrnam_r("absent", &group, buffer, sizeof(buffer), &result) || result) return 3;
    if (getgrgid_r(20, &group, buffer, sizeof(buffer), &result) || !result || strcmp(group.gr_name, "staff")) return 4;
    errno = EBUSY;
    result = getgrnam("staff");
    if (!result || result->gr_gid != 20 || strcmp(result->gr_name, "staff") || strcmp(result->gr_mem[0], "desktop") || errno != EBUSY) return 40;
    result = getgrgid(20);
    if (!result || strcmp(result->gr_name, "staff")) return 41;
    errno = EBUSY;
    if (getgrnam("absent") || getgrgid(65534) || errno != EBUSY) return 42;
    const char *group_names[]={"wheel","staff","messagebus","native-extra","native-duplicate"};
    setgrent();
    for (unsigned i=0;i<sizeof(group_names)/sizeof(*group_names);++i) {
        result=getgrent();
        if (!result || strcmp(result->gr_name,group_names[i])) return 45;
    }
    if (getgrent() || errno!=EBUSY) return 46; /* malformed records skipped, true EOF */
    setgrent();
    result=getgrent();
    if (!result || strcmp(result->gr_name,"wheel")) return 47;
    endgrent();
    result=getgrent();
    if (!result || strcmp(result->gr_name,"wheel")) return 48; /* lazy reopening */
    endgrent();
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) if (pthread_create(&threads[i], NULL, group_worker, (void *)i)) return 43;
    for (unsigned i = 0; i < 4; ++i) {
        void *thread_result;
        if (pthread_join(threads[i], &thread_result) || thread_result) return 44;
    }
    int count = 0, groups[3] = {-99, -99, -99};
    if (getgrouplist("desktop", 20, NULL, &count) != -1 || count != 2) return 5;
    count = 1;
    if (getgrouplist("desktop", 20, groups, &count) != -1 || count != 2 || groups[0] != 20 || groups[1] != -99) return 6;
    count = 3;
    if (getgrouplist("desktop", 20, groups, &count) || count != 2 || groups[0] != 20 || groups[1] != 12345) return 7;
    gid_t before[32], after[32];
    gid_t membership[32];
    if (getgroups(0, NULL) != 2 || getgroups(32, membership) != 2 || membership[0] != 0 || membership[1] != 12345) return 8;
    int original = native_kernel_getgroups(32, before);
    if (original < 0) return 8;
    pid_t child = fork();
    if (child < 0) return 9;
    if (!child) {
        gid_t actual[32];
        if (initgroups("desktop", 20)) _exit(1);
        int n = native_kernel_getgroups(32, actual), base = 0, extra = 0;
        for (int i = 0; i < n; ++i) { base += actual[i] == 20; extra += actual[i] == 12345; }
        _exit(n == 2 && base == 1 && extra == 1 ? 0 : 2);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 10;
    int current = native_kernel_getgroups(32, after);
    if (current != original || memcmp(before, after, (size_t)original * sizeof(gid_t))) return 11;
    return 0;
}

static int codec(const char *nonce)
{
    DBusError error = DBUS_ERROR_INIT;
    DBusMessage *message = dbus_message_new_method_call("org.darwinembedded.NativeBus", "/org/darwinembedded/NativeBus", "org.darwinembedded.NativeBus", "Probe");
    dbus_uint64_t sent = DBUS_UINT64_CONSTANT(0x0123456789abcdef), received = 0;
    if (!message || !dbus_message_append_args(message, DBUS_TYPE_STRING, &nonce, DBUS_TYPE_UINT64, &sent, DBUS_TYPE_INVALID)) return 1;
    dbus_message_set_serial(message, 17);
    char *data;
    int size;
    if (!dbus_message_marshal(message, &data, &size) || size < 64) return 2;
    DBusMessage *copy = dbus_message_demarshal(data, size, &error);
    const char *text = NULL;
    if (!copy || dbus_error_is_set(&error) || !dbus_message_get_args(copy, &error, DBUS_TYPE_STRING, &text,
        DBUS_TYPE_UINT64, &received, DBUS_TYPE_INVALID) || strcmp(text, nonce) || received != sent || dbus_message_get_serial(copy) != 17) return 3;
    dbus_message_unref(copy);
    data[0] = '?';
    copy = dbus_message_demarshal(data, size, &error);
    if (copy || !dbus_error_is_set(&error)) return 4;
    dbus_error_free(&error); dbus_free(data); dbus_message_unref(message);
    if (dbus_signature_validate("a{sv}", &error) != TRUE || dbus_signature_validate("a{", &error) != FALSE || !dbus_error_is_set(&error)) return 5;
    dbus_error_free(&error);
    return 0;
}

static DBusMessage *driver_call(DBusConnection *connection, const char *member, const char *name, DBusError *error)
{
    DBusMessage *request = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", member);
    if (!request) return NULL;
    if (name && !dbus_message_append_args(request, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) { dbus_message_unref(request); return NULL; }
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, request, 10000, error);
    dbus_message_unref(request);
    return reply;
}

static int session(const char *nonce)
{
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    DBusError error = DBUS_ERROR_INIT;
    if (!address || !strstr(address, "unix:") || !dbus_threads_init_default()) return 1;
    DBusConnection *a = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    DBusConnection *b = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!a || !b || dbus_error_is_set(&error)) return 2;
    dbus_connection_set_exit_on_disconnect(a, FALSE); dbus_connection_set_exit_on_disconnect(b, FALSE);
    if (dbus_bus_request_name(b, "org.darwinembedded.NativeBus", 0, &error) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER || dbus_error_is_set(&error)) return 3;
    DBusMessage *reply = driver_call(a, "GetNameOwner", "org.darwinembedded.NativeBus", &error);
    const char *owner;
    if (!reply || !dbus_message_get_args(reply, &error, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) || strcmp(owner, dbus_bus_get_unique_name(b))) return 4;
    dbus_message_unref(reply);
    reply = driver_call(a, "GetConnectionUnixUser", dbus_bus_get_unique_name(b), &error);
    dbus_uint32_t uid;
    if (!reply || !dbus_message_get_args(reply, &error, DBUS_TYPE_UINT32, &uid, DBUS_TYPE_INVALID) || uid != geteuid()) return 5;
    dbus_message_unref(reply);
    reply = driver_call(a, "GetConnectionUnixProcessID", dbus_bus_get_unique_name(b), &error);
    dbus_uint32_t pid;
    if (!reply || !dbus_message_get_args(reply, &error, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID) || pid != (dbus_uint32_t)getpid()) return 6;
    dbus_message_unref(reply);
    dbus_bus_add_match(b, "type='signal',interface='org.darwinembedded.NativeBus',member='Nonce'", &error);
    if (dbus_error_is_set(&error)) return 7;
    DBusMessage *signal = dbus_message_new_signal("/org/darwinembedded/NativeBus", "org.darwinembedded.NativeBus", "Nonce");
    if (!signal || !dbus_message_append_args(signal, DBUS_TYPE_STRING, &nonce, DBUS_TYPE_INVALID) || !dbus_connection_send(a, signal, NULL)) return 8;
    dbus_message_unref(signal); dbus_connection_flush(a);
    int seen = 0;
    for (unsigned i = 0; i < 20 && !seen; ++i) {
        dbus_connection_read_write(b, 500);
        DBusMessage *incoming;
        while ((incoming = dbus_connection_pop_message(b))) {
            if (dbus_message_is_signal(incoming, "org.darwinembedded.NativeBus", "Nonce")) {
                const char *value;
                if (!dbus_message_get_args(incoming, &error, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID) || strcmp(value, nonce)) return 9;
                seen = 1;
            }
            dbus_message_unref(incoming);
        }
    }
    if (!seen) return 10;
    dbus_connection_close(b); dbus_connection_unref(b); dbus_connection_close(a); dbus_connection_unref(a);
    printf("NATIVE_DBUS_SESSION:%s:%d\n", nonce, getpid()); fflush(stdout);
    return 0;
}

static int run_program(const char *program, const char *argument, const char *nonce)
{
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        if (nonce) execl(program, program, "/bin/native-dbus-test", argument, nonce, (char *)NULL);
        else execl(program, program, argument, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 2;
    return 0;
}

static int system_bus(const char *nonce)
{
    if (chown("/var/run/dbus", 81, 81)) return 1;
    int fds[2];
    if (pipe(fds)) return 2;
    pid_t child = fork();
    if (child < 0) return 3;
    if (!child) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(fds[1]);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", "--system", "--nofork", "--nopidfile", "--print-address=1", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char address[512]; size_t length = 0;
    struct pollfd ready = {.fd=fds[0], .events=POLLIN};
    while (length < sizeof(address) - 1) {
        if (poll(&ready, 1, 10000) != 1 || read(fds[0], address + length, 1) != 1) return 4;
        if (address[length++] == '\n') break;
    }
    close(fds[0]);
    if (!length || address[length - 1] != '\n') return 5;
    address[length - 1] = 0;
    DBusError error = DBUS_ERROR_INIT;
    DBusConnection *connection = dbus_connection_open_private(address, &error);
    if (!connection || !dbus_bus_register(connection, &error) || dbus_error_is_set(&error)) {
        printf("Native system bus registration: %s: %s\n", error.name ? error.name : "none", error.message ? error.message : "none");
        fflush(stdout);
        return 6;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    DBusMessage *reply = driver_call(connection, "GetConnectionUnixUser", "org.freedesktop.DBus", &error);
    dbus_uint32_t uid;
    if (!reply || !dbus_message_get_args(reply, &error, DBUS_TYPE_UINT32, &uid, DBUS_TYPE_INVALID) || uid != 81) return 7;
    dbus_message_unref(reply);
    reply = driver_call(connection, "GetConnectionUnixProcessID", "org.freedesktop.DBus", &error);
    dbus_uint32_t pid;
    if (!reply || !dbus_message_get_args(reply, &error, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID) || pid != (dbus_uint32_t)child) return 8;
    dbus_message_unref(reply);
    dbus_connection_close(connection); dbus_connection_unref(connection);
    if (kill(child, SIGTERM)) return 9;
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 10;
    printf("NATIVE_DBUS_SYSTEM:%s:%d:81\n", nonce, child); fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--session")) {
        int error = session(argv[2]);
        return error ? fail("session child", error) : 0;
    }
    struct utsname platform;
    if (argc != 2 || strlen(argv[1]) != 48 || uname(&platform) || getpid() <= 1) return fail("arguments", 1);
    const char *nonce = argv[1];
    printf("NATIVE_DBUS_PID:%s:%d\n", nonce, getpid()); fflush(stdout);
    int error;
    if ((error = accounts())) return fail("native group APIs", error);
    printf("NATIVE_DBUS_STAGE:%s:groups-kernel-credentials\n", nonce); fflush(stdout);
    if ((error = codec(nonce))) return fail("message codec/errors", error);
    printf("NATIVE_DBUS_STAGE:%s:message-codec-errors\n", nonce); fflush(stdout);
    if ((error = run_program("/usr/bin/dbus-uuidgen", "--ensure", NULL))) return fail("machine UUID", error);
    if ((error = run_program("/usr/bin/dbus-run-session", "--session", nonce))) return fail("session runner", error);
    printf("NATIVE_DBUS_STAGE:%s:session-auth-names-signals\n", nonce); fflush(stdout);
    if ((error = system_bus(nonce))) return fail("system daemon identity", error);
    printf("NATIVE_DBUS_STAGE:%s:system-daemon-uid-pid\n", nonce); fflush(stdout);
    printf("NATIVE_DBUS_OK:%s:%s:%s\n", nonce, platform.release, platform.machine); fflush(stdout);
    return 0;
}
