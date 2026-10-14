/* Included by the GIO fixture. A bounded loopback DNS test peer supplies
 * nonce-bearing TXT and SRV/PTR records plus real NXDOMAIN/SERVFAIL replies. */
struct dns_fixture { int socket; gint stop; guint queries; };
static void put16(unsigned char *out, unsigned value)
{
    out[0] = value >> 8; out[1] = value;
}
static guint get16(const unsigned char *in) { return ((guint)in[0] << 8) | in[1]; }
static gpointer dns_peer(gpointer data)
{
    struct dns_fixture *fixture = data;
    while (!g_atomic_int_get(&fixture->stop)) {
        unsigned char query[512], answer[512], record[128];
        struct sockaddr_in client;
        socklen_t client_size = sizeof(client);
        ssize_t size = recvfrom(fixture->socket, query, sizeof(query), 0, (struct sockaddr *)&client, &client_size);
        if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        CHECK(size >= 17 && get16(query + 4) == 1, "actual bounded DNS question");
        size_t offset = 12, used = 0;
        char name[256];
        while (offset < (size_t)size && query[offset]) {
            unsigned length = query[offset++];
            CHECK(length <= 63 && offset + length < (size_t)size && used + length + 1 < sizeof(name), "DNS labels");
            if (used) name[used++] = '.';
            memcpy(name + used, query + offset, length); used += length; offset += length;
        }
        name[used] = 0;
        CHECK(offset + 5 <= (size_t)size, "DNS question fields");
        ++offset;
        unsigned type = get16(query + offset);
        CHECK(get16(query + offset + 2) == 1, "DNS IN class");
        offset += 4;
        unsigned rcode = !strcmp(name, "missing.native.test") ? 3 : !strcmp(name, "temporary.native.test") ? 2 : 0;
        size_t record_length = 0;
        if (!rcode && type == 16 && !strcmp(name, "txt.native.test")) {
            record[0] = (unsigned char)strlen(nonce);
            memcpy(record + 1, nonce, strlen(nonce)); record_length = strlen(nonce) + 1;
        } else if (!rcode && type == 33 && !strcmp(name, "srv.native.test")) {
            put16(record, 3); put16(record + 2, 7); put16(record + 4, 4242);
            const unsigned char target[] = {6,'n','a','t','i','v','e',7,'e','x','a','m','p','l','e',4,'t','e','s','t',0};
            memcpy(record + 6, target, sizeof(target)); record_length = 6 + sizeof(target);
        } else if (!rcode && type == 12 && !strcmp(name, "1.0.0.127.in-addr.arpa")) {
            const unsigned char target[] = {7,'r','e','v','e','r','s','e',6,'n','a','t','i','v','e',4,'t','e','s','t',0};
            memcpy(record, target, sizeof(target)); record_length = sizeof(target);
        } else CHECK(rcode != 0, "expected DNS fixture name/type");
        memcpy(answer, query, offset);
        put16(answer + 2, 0x8180 | rcode); put16(answer + 6, record_length ? 1 : 0);
        put16(answer + 8, 0); put16(answer + 10, 0);
        size_t end = offset;
        if (record_length) {
            put16(answer + end, 0xc00c); put16(answer + end + 2, type); put16(answer + end + 4, 1);
            memset(answer + end + 6, 0, 4); answer[end + 9] = 60;
            put16(answer + end + 10, (unsigned)record_length);
            memcpy(answer + end + 12, record, record_length); end += 12 + record_length;
        }
        CHECK(sendto(fixture->socket, answer, end, 0, (struct sockaddr *)&client, client_size) == (ssize_t)end, "real DNS UDP reply");
        ++fixture->queries;
    }
    return NULL;
}
static gpointer dns_client(gpointer unused)
{
    (void)unused;
    GError *error = NULL;
    GResolver *resolver = g_resolver_get_default();
    GList *records = g_resolver_lookup_records(resolver, "txt.native.test", G_RESOLVER_RECORD_TXT, NULL, &error);
    CALL(records && !records->next, "actual GIO TXT query");
    gchar **strings;
    g_variant_get(records->data, "(^as)", &strings);
    CHECK(strings[0] && !strcmp(strings[0], nonce) && !strings[1], "native DNS TXT nonce bytes");
    g_strfreev(strings); g_list_free_full(records, (GDestroyNotify)g_variant_unref);
    records = g_resolver_lookup_records(resolver, "srv.native.test", G_RESOLVER_RECORD_SRV, NULL, &error);
    CALL(records && !records->next, "actual GIO SRV query");
    guint16 priority, weight, port;
    const gchar *target;
    g_variant_get(records->data, "(qqq&s)", &priority, &weight, &port, &target);
    CHECK(priority == 3 && weight == 7 && port == 4242 && !strcmp(target, "native.example.test"), "real SRV scalar/name parsing");
    g_list_free_full(records, (GDestroyNotify)g_variant_unref);
    records = g_resolver_lookup_records(resolver, "missing.native.test", G_RESOLVER_RECORD_TXT, NULL, &error);
    CHECK(!records && g_error_matches(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND), "actual DNS NXDOMAIN");
    g_clear_error(&error);
    records = g_resolver_lookup_records(resolver, "temporary.native.test", G_RESOLVER_RECORD_TXT, NULL, &error);
    CHECK(!records && g_error_matches(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE), "query-owned DNS SERVFAIL");
    g_clear_error(&error);
    GInetAddress *address = g_inet_address_new_from_string("127.0.0.1");
    gchar *name = g_resolver_lookup_by_address(resolver, address, NULL, &error);
    CALL(name, "actual native reverse DNS");
    CHECK(!strcmp(name, "reverse.native.test"), "actual PTR parsed name");
    g_free(name); g_object_unref(address); g_object_unref(resolver);
    return NULL;
}
static void dns_records(void)
{
    int control = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(control >= 0, "native loopback control socket");
    struct ifreq request = {0};
    memcpy(request.ifr_name, "lo0", 4);
    CHECK(ioctl(control, SIOCGIFFLAGS, &request) == 0, "native loopback flags");
    request.ifr_flags |= IFF_UP;
    CHECK(ioctl(control, SIOCSIFFLAGS, &request) == 0 && ioctl(control, SIOCGIFADDR, &request) == 0, "native loopback up/address");
    struct sockaddr_in *actual = (struct sockaddr_in *)&request.ifr_addr;
    CHECK(actual->sin_family == AF_INET && actual->sin_addr.s_addr == htonl(INADDR_LOOPBACK), "actual lo0 address");
    close(control);
    struct dns_fixture fixture = {.socket = socket(AF_INET, SOCK_DGRAM, 0)};
    struct sockaddr_in local = {.sin_len = sizeof(local), .sin_family = AF_INET,
        .sin_port = htons(53), .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    struct timeval timeout = {.tv_usec = 100000};
    CHECK(fixture.socket >= 0 && setsockopt(fixture.socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
          bind(fixture.socket, (struct sockaddr *)&local, sizeof(local)) == 0, "actual DNS UDP test listener");
    GThread *peer = g_thread_new("dns-peer", dns_peer, &fixture), *clients[4];
    for (unsigned i = 0; i < 4; ++i) clients[i] = g_thread_new("gio-dns", dns_client, NULL);
    for (unsigned i = 0; i < 4; ++i) g_thread_join(clients[i]);
    g_atomic_int_set(&fixture.stop, TRUE); g_thread_join(peer);
    CHECK(fixture.queries == 20, "actual concurrent DNS query count");
    close(fixture.socket);
    puts("GIO_DNS_TXT_SRV_PTR_CONCURRENT_ERRORS_OK");
}
