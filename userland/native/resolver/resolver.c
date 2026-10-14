/* Synchronous Darwin API over the pinned OpenBSD asynchronous resolver.
 * All parser/context transitions run with cancellation disabled. Cancellation
 * is enabled only while waiting on the actual DNS socket, with the query and
 * its descriptors owned by a cleanup handler. No global BIND _res is exported. */
#include "native_config.h"
#include <asr.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <string.h>
#include "records.h"

struct query_owner {
    struct asr_query *query;
    struct addrinfo *answer;
    void *packet;
};

static void cancel_query(void *context)
{
    struct query_owner *owner = context;
    int ignored;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &ignored);
    if (owner->query) asr_abort(owner->query);
    if (owner->answer) freeaddrinfo(owner->answer);
    free(owner->packet);
}

/* Preserve the DNS timeout across EINTR using the real monotonic clock. */
static int wait_socket(const struct asr_result *answer, int cancel_state)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    struct pollfd fd = {answer->ar_fd,
        answer->ar_cond == ASR_WANT_READ ? POLLIN : POLLOUT, 0};
    int timeout = answer->ar_timeout;
    for (;;) {
        pthread_setcancelstate(cancel_state, NULL);
        int result = poll(&fd, 1, timeout);
        int error = errno;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (result != -1 || error != EINTR) { errno = error; return result; }
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        int64_t elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= answer->ar_timeout) return 0;
        timeout = answer->ar_timeout - (int)elapsed;
    }
}

int getaddrinfo(const char *host, const char *service,
               const struct addrinfo *hints, struct addrinfo **result)
{
    if (!result) { errno = EINVAL; return EAI_SYSTEM; }
    *result = NULL;
    int saved_errno = errno, cancel_state, status = EAI_SYSTEM, error = 0;
    int state_error = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    if (state_error) { errno = state_error; return EAI_SYSTEM; }
    struct query_owner owner = {0};
    struct asr_result answer;
    pthread_cleanup_push(cancel_query, &owner);
    owner.query = getaddrinfo_async(host, service, hints, NULL);
    if (!owner.query) {
        error = errno;
        status = error == ENOMEM ? EAI_MEMORY : EAI_SYSTEM;
    } else {
        for (;;) {
            if (asr_run(owner.query, &answer)) {
                owner.query = NULL; /* asr_run has destroyed the finished query */
                owner.answer = answer.ar_addrinfo;
                status = answer.ar_gai_errno;
                error = answer.ar_errno;
                break;
            }
            if (wait_socket(&answer, cancel_state) == -1) {
                error = errno;
                asr_abort(owner.query);
                owner.query = NULL;
                break;
            }
        }
    }
    /* A pending deferred cancellation still releases the completed result. */
    pthread_setcancelstate(cancel_state, NULL);
    pthread_testcancel();
    *result = owner.answer;
    owner.answer = NULL;
    pthread_cleanup_pop(0);
    errno = status == EAI_SYSTEM ? error : saved_errno;
    return status;
}

/* Drive the same real UDP/TCP state machine with cancellation ownership. */
static int finish_query(struct query_owner *owner, struct asr_result *answer,
                        int cancel_state)
{
    while (!asr_run(owner->query, answer)) {
        if (wait_socket(answer, cancel_state) == -1) {
            int error = errno;
            asr_abort(owner->query);
            owner->query = NULL;
            errno = error;
            return -1;
        }
    }
    owner->query = NULL;
    return 0;
}

/* The original ASR parser allocates the complete hostent, including aliases
 * and address vectors, in one block. Each thread retains its own last answer.
 * Apple's herror.c supplies Darwin's public h_errno and error messages. */
struct host_state { struct hostent *answer; };
static pthread_once_t host_once = PTHREAD_ONCE_INIT;
static pthread_key_t host_key;
static int host_key_error;
static void free_host(void *pointer)
{
    struct host_state *state = pointer;
    free(state->answer);
    free(state);
}
static void create_host_key(void)
{
    host_key_error = pthread_key_create(&host_key, free_host);
}
static struct hostent *lookup_host(const char *name, const void *address, socklen_t length, int family)
{
    int saved_errno = errno, cancel_state;
    int error = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    if (error) { errno = error; h_errno = NETDB_INTERNAL; return NULL; }
    error = pthread_once(&host_once, create_host_key);
    if (!error) error = host_key_error;
    struct host_state *state = error ? NULL : pthread_getspecific(host_key);
    if (!state && !error) {
        state = calloc(1, sizeof(*state));
        if (!state) error = ENOMEM;
        else {
            error = pthread_setspecific(host_key, state);
            if (error) { free(state); state = NULL; }
        }
    }
    struct query_owner owner = {0};
    struct asr_result answer;
    struct hostent *result = NULL;
    int host_error = NETDB_INTERNAL;
    pthread_cleanup_push(cancel_query, &owner);
    if (!error) {
        owner.query = name ? gethostbyname2_async(name, family, NULL) : gethostbyaddr_async(address, length, family, NULL);
        if (!owner.query || finish_query(&owner, &answer, cancel_state)) error = errno;
        else {
            owner.packet = answer.ar_hostent;
            error = answer.ar_errno;
            host_error = answer.ar_h_errno;
            if (!error && !host_error && owner.packet) {
                free(state->answer);
                state->answer = owner.packet;
                owner.packet = NULL;
                result = state->answer;
            }
        }
    }
    free(owner.packet); owner.packet = NULL;
    h_errno = host_error;
    errno = error ? error : saved_errno;
    pthread_setcancelstate(cancel_state, NULL);
    pthread_testcancel();
    pthread_cleanup_pop(0);
    return result;
}
struct hostent *gethostbyname2(const char *name, int family)
{
    if (!name) { errno = EINVAL; h_errno = NETDB_INTERNAL; return NULL; }
    return lookup_host(name, NULL, 0, family);
}
struct hostent *gethostbyname(const char *name)
{
    return gethostbyname2(name, AF_INET);
}
struct hostent *gethostbyaddr(const void *address, socklen_t length, int family)
{
    if (!address) { errno = EINVAL; h_errno = NETDB_INTERNAL; return NULL; }
    return lookup_host(NULL, address, length, family);
}

int native_resolver_query(const char *name, int query_class, int type,
                          unsigned char *buffer, int capacity, int *query_error)
{
    if (!query_error) { errno = EINVAL; return -1; }
    *query_error = NO_RECOVERY;
    if (!name || !buffer || capacity <= 0 || query_class < 0 || query_class > 65535 ||
        type < 0 || type > 65535) { errno = EINVAL; return -1; }
    int cancel_state, status = -1, saved_errno = errno, error = 0;
    int state_error = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    if (state_error) { errno = state_error; return -1; }
    struct query_owner owner = {0};
    struct asr_result answer;
    pthread_cleanup_push(cancel_query, &owner);
    owner.query = res_query_async(name, query_class, type, NULL);
    if (!owner.query || finish_query(&owner, &answer, cancel_state)) {
        error = errno;
        *query_error = NETDB_INTERNAL;
    } else {
        owner.packet = answer.ar_data;
        error = answer.ar_errno;
        *query_error = answer.ar_h_errno;
        if (!error && !answer.ar_h_errno && answer.ar_datalen >= 0 && owner.packet) {
            int copied = answer.ar_datalen < capacity ? answer.ar_datalen : capacity;
            memcpy(buffer, owner.packet, (size_t)copied);
            status = answer.ar_datalen; /* The caller can retry with a larger buffer. */
        }
    }
    free(owner.packet);
    owner.packet = NULL;
    pthread_setcancelstate(cancel_state, NULL);
    pthread_testcancel();
    pthread_cleanup_pop(0);
    errno = error ? error : saved_errno;
    return status;
}

int getnameinfo(const struct sockaddr *address, socklen_t length,
                char *host, socklen_t host_size, char *service, socklen_t service_size,
                int flags)
{
    if (!address || length < sizeof(address->sa_len) + sizeof(address->sa_family))
        return EAI_FAIL;
    if (address->sa_family != AF_INET && address->sa_family != AF_INET6) return EAI_FAMILY;
    size_t required = address->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    /* Darwin's public API uses its explicit salen argument. POSIX callers
     * need not initialize the BSD sa_len byte; ASR owns a normalized copy. */
    if (length != required) return EAI_FAIL;
    if (flags & ~(NI_NUMERICHOST | NI_NAMEREQD | NI_NUMERICSERV | NI_DGRAM)) return EAI_BADFLAGS;
    if ((!host || !host_size) && (!service || !service_size)) return EAI_NONAME;
    int cancel_state, status = EAI_SYSTEM, saved_errno = errno, error = 0;
    int state_error = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    if (state_error) { errno = state_error; return EAI_SYSTEM; }
    struct query_owner owner = {0};
    struct asr_result answer;
    pthread_cleanup_push(cancel_query, &owner);
    owner.query = getnameinfo_async(address, length, host, host_size, service, service_size, flags, NULL);
    if (!owner.query || finish_query(&owner, &answer, cancel_state)) {
        error = errno;
    } else {
        status = answer.ar_gai_errno;
        error = answer.ar_errno;
    }
    pthread_setcancelstate(cancel_state, NULL);
    pthread_testcancel();
    pthread_cleanup_pop(0);
    errno = status == EAI_SYSTEM ? error : saved_errno;
    return status;
}

struct service_state {
    struct servent record;
    struct servent_data parser;
};
static pthread_once_t service_once = PTHREAD_ONCE_INIT;
static pthread_key_t service_key;
static int service_error;
static void free_service(void *pointer)
{
    struct service_state *state = pointer;
    endservent_r(&state->parser);
    free(state);
}
static void create_service_key(void)
{
    service_error = pthread_key_create(&service_key, free_service);
}
static struct servent *lookup_service(const char *name, const char *protocol)
{
    if (!name) { errno = EINVAL; return NULL; }
    int error = pthread_once(&service_once, create_service_key);
    if (!error) error = service_error;
    if (error) { errno = error; return NULL; }
    struct service_state *state = pthread_getspecific(service_key);
    if (!state) {
        state = calloc(1, sizeof(*state));
        if (!state) return NULL;
        error = pthread_setspecific(service_key, state);
        if (error) { free(state); errno = error; return NULL; }
    }
    return getservbyname_r(name, protocol, &state->record, &state->parser) ? NULL : &state->record;
}

struct servent *getservbyname(const char *name, const char *protocol)
{
    int cancel_state, error = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    if (error) { errno = error; return NULL; }
    struct servent *result = lookup_service(name, protocol);
    error = errno;
    pthread_setcancelstate(cancel_state, NULL);
    pthread_testcancel();
    errno = error;
    return result;
}
