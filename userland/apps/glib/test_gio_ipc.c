/* Included by the isolated GIO probe: an actual authenticated Unix D-Bus
 * peer, with real method dispatch, credentials and remote error handling. */
struct dbus_fixture {
    struct event_wait wait;
    GDBusNodeInfo *introspection;
    const gchar *address;
    GDBusConnection *connection;
    guint registration;
    gboolean called;
};
static void echo_method(GDBusConnection *connection, const gchar *sender,
                        const gchar *path, const gchar *interface, const gchar *method,
                        GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface;
    struct dbus_fixture *fixture = data;
    const gchar *message;
    CHECK(!strcmp(method, "Echo"), "actual D-Bus method");
    g_variant_get(parameters, "(&s)", &message);
    CHECK(!strcmp(message, nonce), "actual D-Bus request nonce");
    gchar *reply = g_strdup_printf("%s:%ld", message, (long)getpid());
    fixture->called = TRUE;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", reply));
    g_free(reply);
}
static const GDBusInterfaceVTable echo_vtable = {.method_call = echo_method};
static gboolean peer_connected(GDBusServer *server, GDBusConnection *connection, gpointer data)
{
    (void)server;
    struct dbus_fixture *fixture = data;
    GError *error = NULL;
    GCredentials *credentials = g_dbus_connection_get_peer_credentials(connection);
    CALL(credentials, "authenticated D-Bus peer credentials");
    CHECK(g_credentials_get_unix_user(credentials, &error) == getuid() && !error, "D-Bus actual peer UID");
    fixture->registration = g_dbus_connection_register_object(connection, "/org/native/Probe",
        fixture->introspection->interfaces[0], &echo_vtable, fixture, NULL, &error);
    CALL(fixture->registration != 0, "real D-Bus object registration");
    fixture->connection = g_object_ref(connection);
    return TRUE;
}
static gboolean dbus_finished(gpointer data)
{
    struct dbus_fixture *fixture = data;
    fixture->wait.seen = TRUE;
    g_main_loop_quit(fixture->wait.loop);
    return G_SOURCE_REMOVE;
}
static gpointer dbus_client(gpointer data)
{
    struct dbus_fixture *fixture = data;
    GError *error = NULL;
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(fixture->address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &error);
    CALL(connection, "real D-Bus EXTERNAL authentication");
    GVariant *result = g_dbus_connection_call_sync(connection, NULL, "/org/native/Probe", "org.native.Probe",
        "Echo", g_variant_new("(s)", nonce), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    CALL(result, "real D-Bus round trip");
    const gchar *reply;
    g_variant_get(result, "(&s)", &reply);
    gchar *expected = g_strdup_printf("%s:%ld", nonce, (long)getpid());
    CHECK(!strcmp(reply, expected), "D-Bus decoded reply bytes");
    g_free(expected); g_variant_unref(result);
    result = g_dbus_connection_call_sync(connection, NULL, "/org/native/Probe", "org.native.Probe",
        "Missing", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    CHECK(!result && error && g_dbus_error_is_remote_error(error) &&
          g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD), "actual remote UnknownMethod error");
    g_clear_error(&error);
    CALL(g_dbus_connection_close_sync(connection, NULL, &error), "D-Bus client close");
    g_object_unref(connection);
    g_main_context_invoke(NULL, dbus_finished, fixture);
    return NULL;
}
static void dbus_ipc(void)
{
    GError *error = NULL;
    struct dbus_fixture fixture = {.wait = {g_main_loop_new(NULL, FALSE), FALSE}};
    fixture.introspection = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.native.Probe'><method name='Echo'>"
        "<arg type='s' direction='in'/><arg type='s' direction='out'/>"
        "</method></interface></node>", &error);
    CALL(fixture.introspection, "D-Bus introspection parse");
    gchar *guid = g_dbus_generate_guid();
    GDBusServer *server = g_dbus_server_new_sync("unix:tmpdir=/tmp", G_DBUS_SERVER_FLAGS_NONE,
                                               guid, NULL, NULL, &error);
    CALL(server, "actual Darwin Unix D-Bus server");
    g_free(guid);
    g_signal_connect(server, "new-connection", G_CALLBACK(peer_connected), &fixture);
    g_dbus_server_start(server);
    fixture.address = g_dbus_server_get_client_address(server);
    GThread *client = g_thread_new("native-dbus", dbus_client, &fixture);
    GSource *timeout = g_timeout_source_new_seconds(8);
    g_source_set_callback(timeout, timed_out, &fixture.wait, NULL);
    g_source_attach(timeout, NULL);
    g_main_loop_run(fixture.wait.loop);
    CHECK(fixture.wait.seen && fixture.called && fixture.connection, "D-Bus authenticated dispatch completion");
    g_thread_join(client);
    g_source_destroy(timeout); g_source_unref(timeout);
    CHECK(g_dbus_connection_unregister_object(fixture.connection, fixture.registration), "D-Bus unregister");
    if (!g_dbus_connection_is_closed(fixture.connection))
        CALL(g_dbus_connection_close_sync(fixture.connection, NULL, &error), "D-Bus server connection close");
    g_object_unref(fixture.connection);
    g_dbus_server_stop(server); g_object_unref(server);
    g_dbus_node_info_unref(fixture.introspection);
    g_main_loop_unref(fixture.wait.loop);
    puts("GIO_DBUS_AUTH_ROUNDTRIP_ERRORS_OK");
}
