/* Native Darwin integration test. The local DNS peer speaks real UDP/TCP;
 * answers are test data, not a replacement for the resolver under test. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

extern int native_x11_check(void);

static atomic_int stopped, server_error, silent_seen;
static atomic_uint udp_queries, tcp_queries, noise_queries;
static int udp_socket = -1, tcp_socket = -1;

static int failure(int code)
{
    printf("Native resolver failure: %d (errno=%d)\n", code, errno);
    fflush(stdout);
    return 1;
}

static int64_t milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned read16(const unsigned char *p)
{
    return (unsigned)p[0] * 256 + p[1];
}

static void write16(unsigned char *p, unsigned n)
{
    p[0] = (unsigned char)(n >> 8);
    p[1] = (unsigned char)n;
}

/* Bound all framing and label reads; the peer accepts one IN question only. */
static int question(const unsigned char *packet, size_t length, char *name,
                    size_t *end, unsigned *type)
{
    if (length < 17 || read16(packet + 4) != 1 || (packet[2] & 0xf8)) return -1;
    size_t offset = 12, used = 0;
    while (offset < length && packet[offset]) {
        unsigned count = packet[offset++];
        if (count > 63 || count > length - offset || used + count + 1 >= 256) return -1;
        if (used) name[used++] = '.';
        memcpy(name + used, packet + offset, count);
        used += count;
        offset += count;
    }
    if (offset >= length || !used || length - ++offset < 4) return -1;
    name[used] = 0;
    *type = read16(packet + offset);
    if (read16(packet + offset + 2) != 1) return -1;
    *end = offset + 4;
    return 0;
}

static size_t encode_name(unsigned char *out, const char *name)
{
    size_t used = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t count = dot ? (size_t)(dot - name) : strlen(name);
        out[used++] = (unsigned char)count;
        memcpy(out + used, name, count);
        used += count;
        if (!dot) break;
        name = dot + 1;
    }
    out[used++] = 0;
    return used;
}

static size_t add_record(unsigned char *packet, size_t used, const char *owner,
                         unsigned type, const void *data, unsigned length)
{
    if (owner) used += encode_name(packet + used, owner);
    else { packet[used++] = 0xc0; packet[used++] = 12; }
    write16(packet + used, type);
    write16(packet + used + 2, 1);
    memset(packet + used + 4, 0, 4); /* TTL zero: every call reaches the peer. */
    write16(packet + used + 8, length);
    memcpy(packet + used + 10, data, length);
    return used + 10 + length;
}

static size_t reply(const unsigned char *query, size_t length,
                    unsigned char *answer, int tcp, int *silent)
{
    char name[256];
    size_t used;
    unsigned type;
    if (question(query, length, name, &used, &type)) return 0;
    memcpy(answer, query, used);
    answer[2] = 0x81;
    answer[3] = 0x80;
    memset(answer + 6, 0, 6);
    *silent = !strcmp(name, "silent.native.test");
    if (*silent) { atomic_store(&silent_seen, 1); return 0; }
    if (!strcmp(name, "61.2.0.192.in-addr.arpa") && type == 12) {
        unsigned char encoded[256];
        size_t size = encode_name(encoded, "a.native.test");
        used = add_record(answer, used, NULL, 12, encoded, (unsigned)size);
        write16(answer + 6, 1);
        return used;
    }
    if (!strcmp(name, "missing.native.test")) { answer[3] |= 3; return used; }
    if (!strcmp(name, "failure.native.test")) { answer[3] |= 2; return used; }
    if (!strcmp(name, "tcp.native.test") && !tcp) {
        answer[2] |= 2; /* A real truncated UDP response requires TCP retry. */
        return used;
    }
    unsigned last = 61;
    if (!strcmp(name, "tcp.native.test")) last = 62;
    else if (!strncmp(name, "parallel", 8) && name[8] >= '0' && name[8] <= '3' &&
             !strcmp(name + 9, ".native.test")) last = 70 + (unsigned)(name[8] - '0');
    else if (strcmp(name, "a.native.test") && strcmp(name, "aaaa.native.test") &&
             strcmp(name, "alias.native.test") && strcmp(name, "noise.native.test")) {
        answer[3] |= 3;
        return used;
    }
    const char *canonical = !strcmp(name, "alias.native.test") ? "canonical.native.test" : NULL;
    unsigned count = 0;
    if (canonical) {
        unsigned char encoded[256];
        size_t size = encode_name(encoded, canonical);
        used = add_record(answer, used, NULL, 5, encoded, (unsigned)size);
        count++;
    }
    if (type == 1) {
        unsigned char address[] = {192, 0, 2, (unsigned char)last};
        used = add_record(answer, used, canonical, 1, address, sizeof(address));
        count++;
    } else if (type == 28) {
        unsigned char address[16] = {0x20, 1, 0x0d, 0xb8};
        address[15] = 0x61;
        used = add_record(answer, used, canonical, 28, address, sizeof(address));
        count++;
    }
    write16(answer + 6, count);
    if (!strcmp(name, "noise.native.test")) {
        unsigned attempt = atomic_fetch_add(&noise_queries, 1);
        if (attempt == 0) answer[1] ^= 0x80; /* wrong transaction, must be rejected */
        if (attempt == 1) used = 5;          /* malformed header, must be rejected */
    }
    return used;
}

static int transfer(int fd, unsigned char *data, size_t length, int writing)
{
    int64_t deadline = milliseconds() + 3000;
    while (length) {
        int64_t remaining = deadline - milliseconds();
        if (remaining <= 0) return -1;
        struct pollfd pfd = {fd, writing ? POLLOUT : POLLIN, 0};
        int ready = poll(&pfd, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t amount = writing ? send(fd, data, length, 0) : recv(fd, data, length, 0);
        if (amount < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (amount <= 0) return -1;
        data += amount;
        length -= (size_t)amount;
    }
    return 0;
}

static void *serve(void *unused)
{
    (void)unused;
    int64_t deadline = milliseconds() + 90000;
    while (!atomic_load(&stopped)) {
        if (milliseconds() >= deadline) {
            failure(900); /* Bound even an accidentally stuck resolver caller. */
            _exit(1);
        }
        struct pollfd fds[2] = {{udp_socket, POLLIN, 0}, {tcp_socket, POLLIN, 0}};
        int ready = poll(fds, 2, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { atomic_store(&server_error, 1); break; }
        if (fds[0].revents & POLLIN) {
            unsigned char query[1024], answer[1024];
            struct sockaddr_in peer;
            socklen_t size = sizeof(peer);
            ssize_t got = recvfrom(udp_socket, query, sizeof(query), 0, (struct sockaddr *)&peer, &size);
            int silent = 0;
            if (got < 0) { atomic_store(&server_error, 2); break; }
            atomic_fetch_add(&udp_queries, 1);
            size_t length = reply(query, (size_t)got, answer, 0, &silent);
            if ((!length && !silent) || (length && sendto(udp_socket, answer, length, 0,
                    (struct sockaddr *)&peer, size) != (ssize_t)length)) {
                atomic_store(&server_error, 3); break;
            }
        }
        if (fds[1].revents & POLLIN) {
            int client = accept(tcp_socket, NULL, NULL);
            if (client < 0) { atomic_store(&server_error, 4); break; }
            unsigned char prefix[2], query[1024], answer[1024];
            int error = transfer(client, prefix, 2, 0);
            unsigned length = error ? 0 : read16(prefix);
            if (!length || length > sizeof(query)) error = 1;
            if (!error) error = transfer(client, query, length, 0);
            int silent = 0;
            size_t out = error ? 0 : reply(query, length, answer, 1, &silent);
            if (!out) error = 1;
            write16(prefix, (unsigned)out);
            if (!error) error = transfer(client, prefix, 2, 1) || transfer(client, answer, out, 1);
            close(client);
            if (error) { atomic_store(&server_error, 5); break; }
            atomic_fetch_add(&tcp_queries, 1);
        }
    }
    return NULL;
}

static int start_server(pthread_t *thread)
{
    int control = socket(AF_INET, SOCK_DGRAM, 0);
    if (control < 0) return 1;
    struct ifreq request = {0};
    memcpy(request.ifr_name, "lo0", 4);
    if (ioctl(control, SIOCGIFFLAGS, &request)) { close(control); return 2; }
    request.ifr_flags |= IFF_UP;
    if (ioctl(control, SIOCSIFFLAGS, &request)) { close(control); return 3; }
    /* XNU normally assigns loopback itself. Verify the actual address. */
    if (ioctl(control, SIOCGIFADDR, &request)) { close(control); return 4; }
    const struct sockaddr_in *loop = (const struct sockaddr_in *)&request.ifr_addr;
    if (loop->sin_family != AF_INET || loop->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        close(control); return 5;
    }
    close(control);
    struct sockaddr_in local = {0};
    local.sin_len = sizeof(local);
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (udp_socket < 0 || tcp_socket < 0) return 6;
    int one = 1;
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(udp_socket, (struct sockaddr *)&local, sizeof(local)) ||
        bind(tcp_socket, (struct sockaddr *)&local, sizeof(local)) || listen(tcp_socket, 8)) return 7;
    return pthread_create(thread, NULL, serve, NULL) ? 8 : 0;
}

static int lookup(const char *name, const char *service, int family, int kind,
                  int flags, const char *expected, unsigned port, const char *canonical)
{
    struct addrinfo hints = {0}, *answer = NULL;
    hints.ai_family = family;
    hints.ai_socktype = kind;
    hints.ai_flags = flags;
    int error = getaddrinfo(name, service, &hints, &answer);
    if (error || !answer) return 1;
    unsigned char address[16];
    if (inet_pton(family, expected, address) != 1) { freeaddrinfo(answer); return 2; }
    if (canonical && (!answer->ai_canonname || strcmp(answer->ai_canonname, canonical))) {
        freeaddrinfo(answer); return 3;
    }
    for (struct addrinfo *item = answer; item; item = item->ai_next) {
        if (item->ai_family != family || item->ai_socktype != kind || !item->ai_addr) { error = 4; break; }
        if (family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)item->ai_addr;
            if (item->ai_addrlen != sizeof(*in) || in->sin_len != sizeof(*in) ||
                in->sin_family != AF_INET || ntohs(in->sin_port) != port ||
                memcmp(&in->sin_addr, address, 4)) { error = 5; break; }
        } else {
            const struct sockaddr_in6 *in = (const struct sockaddr_in6 *)item->ai_addr;
            if (item->ai_addrlen != sizeof(*in) || in->sin6_len != sizeof(*in) ||
                in->sin6_family != AF_INET6 || ntohs(in->sin6_port) != port ||
                memcmp(&in->sin6_addr, address, 16)) { error = 6; break; }
        }
    }
    freeaddrinfo(answer);
    return error;
}

static int reject(const char *host, const char *service, int flags, int expected)
{
    struct addrinfo hints = {0}, *answer = (struct addrinfo *)(uintptr_t)1;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;
    int error = getaddrinfo(host, service, &hints, &answer);
    if (!error || answer != NULL) {
        fprintf(stderr, "Native resolver failure: rejection %s: error=%d result=%p\n", host, error, (void *)answer);
        return 1;
    }
    if (expected && error != expected) {
        fprintf(stderr, "Native resolver failure: rejection %s: error=%d expected=%d\n", host, error, expected);
        return 2;
    }
    if (!gai_strerror(error) || !*gai_strerror(error)) return 3;
    return 0;
}

struct worker { unsigned index; int result; };

static int host_matches(struct hostent *answer, int family, const char *address, const char *canonical)
{
    unsigned char expected[16];
    int length = family == AF_INET ? 4 : 16;
    return answer && answer->h_addrtype == family && answer->h_length == length &&
        answer->h_addr_list && answer->h_addr_list[0] && !answer->h_addr_list[1] &&
        inet_pton(family, address, expected) == 1 && !memcmp(answer->h_addr_list[0], expected, (size_t)length) &&
        answer->h_name && !strcmp(answer->h_name, canonical);
}

static void *parallel_lookup(void *opaque)
{
    struct worker *worker = opaque;
    char name[] = "parallel0.native.test", address[] = "192.0.2.70";
    name[8] += (char)worker->index;
    address[9] += (char)worker->index;
    worker->result = lookup(name, "4242", AF_INET, SOCK_STREAM, 0, address, 4242, NULL);
    if (!worker->result) {
        struct hostent *answer = gethostbyname(name);
        if (usleep(10000) || !host_matches(answer, AF_INET, address, name)) worker->result = 1;
    }
    return NULL;
}

static void *silent_lookup(void *unused)
{
    (void)unused;
    struct addrinfo hints = {0}, *answer = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    (void)getaddrinfo("silent.native.test", "4242", &hints, &answer);
    if (answer) freeaddrinfo(answer);
    return NULL;
}

static void *silent_host_lookup(void *unused)
{
    (void)unused;
    (void)gethostbyname("silent.native.test");
    return NULL;
}

static int descriptor_count(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) || limit.rlim_cur > 65536) return -1;
    int count = 0;
    for (int fd = 0; (rlim_t)fd < limit.rlim_cur; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) count++;
        else if (errno != EBADF) return -1;
    }
    return count;
}

static int check_resolver(void)
{
    fprintf(stderr, "Resolver stage: numeric and files\n");
    if (lookup("192.0.2.41", "4242", AF_INET, SOCK_STREAM,
               AI_NUMERICHOST | AI_NUMERICSERV, "192.0.2.41", 4242, NULL)) return 10;
    if (lookup("2001:db8::41", "4242", AF_INET6, SOCK_STREAM,
               AI_NUMERICHOST | AI_NUMERICSERV, "2001:db8::41", 4242, NULL)) return 11;
    if (lookup(NULL, "4242", AF_INET, SOCK_STREAM, AI_PASSIVE, "0.0.0.0", 4242, NULL) ||
        lookup(NULL, "4242", AF_INET6, SOCK_STREAM, AI_PASSIVE, "::", 4242, NULL)) return 12;
    if (reject("192.0.2.41", "4242", AI_V4MAPPED, EAI_BADFLAGS) ||
        reject("192.0.2.41", "4242", AI_ALL, EAI_BADFLAGS)) return 13;
    if (reject("bad.invalid", "4242", AI_NUMERICHOST, EAI_NONAME) ||
        reject("192.0.2.41", "native-service", AI_NUMERICSERV, EAI_SERVICE) ||
        reject("192.0.2.41", "65536", AI_NUMERICSERV, 0)) return 14;
    unsigned before = atomic_load(&udp_queries);
    if (lookup("native-host-alias", "native-service-alias", AF_INET, SOCK_STREAM,
               AI_CANONNAME, "192.0.2.45", 4242, "native-host.example")) return 15;
    if (lookup("native-v6-alias", "native-dgram-alias", AF_INET6, SOCK_DGRAM,
               AI_CANONNAME, "2001:db8::45", 4243, "native-v6.example")) return 16;
    if (atomic_load(&udp_queries) != before) return 17;
    if (!host_matches(gethostbyname("native-host-alias"), AF_INET, "192.0.2.45", "native-host.example") ||
        !host_matches(gethostbyname2("native-v6-alias", AF_INET6), AF_INET6, "2001:db8::45", "native-v6.example")) return 18;
    if (atomic_load(&udp_queries) != before) return 19;
    fprintf(stderr, "Resolver stage: DNS records and errors\n");
    if (lookup("a.native.test", "4242", AF_INET, SOCK_STREAM,
               AI_CANONNAME, "192.0.2.61", 4242, "a.native.test")) return 20;
    if (lookup("aaaa.native.test", "4242", AF_INET6, SOCK_STREAM,
               AI_CANONNAME, "2001:db8::61", 4242, "aaaa.native.test")) return 21;
    if (lookup("alias.native.test", "4242", AF_INET, SOCK_STREAM,
               AI_CANONNAME, "192.0.2.61", 4242, "canonical.native.test")) return 22;
    if (reject("missing.native.test", "4242", 0, 0)) return 23;
    if (reject("failure.native.test", "4242", 0, EAI_AGAIN)) return 24;
    if (!host_matches(gethostbyname("alias.native.test"), AF_INET, "192.0.2.61", "canonical.native.test")) return 50;
    if (!host_matches(gethostbyname2("aaaa.native.test", AF_INET6), AF_INET6, "2001:db8::61", "aaaa.native.test")) return 51;
    struct in_addr reverse;
    if (inet_pton(AF_INET, "192.0.2.61", &reverse) != 1 ||
        !host_matches(gethostbyaddr(&reverse, sizeof(reverse), AF_INET), AF_INET, "192.0.2.61", "a.native.test")) return 52;
    if (gethostbyname("missing.native.test") || h_errno != HOST_NOT_FOUND) return 53;
    if (gethostbyname("failure.native.test") || h_errno != TRY_AGAIN) return 54;
    if (gethostbyname2("a.native.test", AF_UNIX) || errno != EAFNOSUPPORT) return 55;
    if (gethostbyaddr(&reverse, 1, AF_INET) || errno != EINVAL) return 56;
    fprintf(stderr, "Resolver stage: TCP fallback and rejected replies\n");
    if (lookup("tcp.native.test", "4242", AF_INET, SOCK_STREAM, 0,
               "192.0.2.62", 4242, NULL)) return 25;
    int64_t tcp_deadline = milliseconds() + 3000;
    while (!atomic_load(&tcp_queries)) {
        if (milliseconds() >= tcp_deadline || atomic_load(&server_error)) return 25;
        poll(NULL, 0, 1);
    }
    if (lookup("noise.native.test", "4242", AF_INET, SOCK_STREAM, 0,
               "192.0.2.61", 4242, NULL) || atomic_load(&noise_queries) < 3) return 26;
    fprintf(stderr, "Resolver stage: parallel contexts\n");
    pthread_t threads[4];
    struct worker workers[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){i, -1};
        if (pthread_create(&threads[i], NULL, parallel_lookup, &workers[i])) return 30;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (pthread_join(threads[i], NULL) || workers[i].result) return 31;
    fprintf(stderr, "Resolver stage: cancellation and descriptor cleanup\n");
    int descriptors = descriptor_count();
    if (descriptors < 0) return 32;
    pthread_t cancelled;
    if (pthread_create(&cancelled, NULL, silent_lookup, NULL)) return 33;
    int64_t deadline = milliseconds() + 5000;
    while (!atomic_load(&silent_seen)) {
        if (milliseconds() >= deadline || atomic_load(&server_error)) return 34;
        poll(NULL, 0, 10);
    }
    void *result;
    if (pthread_cancel(cancelled) || pthread_join(cancelled, &result) || result != PTHREAD_CANCELED) return 35;
    if (descriptor_count() != descriptors) return 36;
    atomic_store(&silent_seen, 0);
    if (pthread_create(&cancelled, NULL, silent_host_lookup, NULL)) return 57;
    deadline = milliseconds() + 5000;
    while (!atomic_load(&silent_seen)) {
        if (milliseconds() >= deadline || atomic_load(&server_error)) return 58;
        poll(NULL, 0, 10);
    }
    if (pthread_cancel(cancelled) || pthread_join(cancelled, &result) || result != PTHREAD_CANCELED) return 59;
    if (descriptor_count() != descriptors) return 60;
    if (lookup("a.native.test", "4242", AF_INET, SOCK_STREAM, 0, "192.0.2.61", 4242, NULL)) return 37;
    return atomic_load(&server_error) ? 40 + atomic_load(&server_error) : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) != 48 || strspn(argv[1], "0123456789abcdef") != 48) return failure(1);
    pthread_t server;
    int error = start_server(&server);
    if (error) return failure(100 + error);
    error = check_resolver();
    atomic_store(&stopped, 1);
    if (pthread_join(server, NULL) && !error) error = 2;
    close(udp_socket);
    close(tcp_socket);
    if (error) return failure(error);
    fprintf(stderr, "Resolver stage: native X11 clients\n");
    error = native_x11_check();
    if (error) return failure(1000 + error);
    char release[128], machine[128];
    int mib[2] = {CTL_KERN, KERN_OSRELEASE};
    size_t size = sizeof(release);
    if (sysctl(mib, 2, release, &size, NULL, 0) || !size ||
        size > sizeof(release) || release[size - 1]) return failure(3);
    mib[0] = CTL_HW;
    mib[1] = HW_MACHINE;
    size = sizeof(machine);
    if (sysctl(mib, 2, machine, &size, NULL, 0) || !size ||
        size > sizeof(machine) || machine[size - 1]) return failure(4);
    printf("NATIVE_RESOLVER_OK:%s:%s:%s\n", argv[1], release, machine);
    exit(0);
}
