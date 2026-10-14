/* Native Darwin account lookup for roots using passwd files.
 * Records come from disk. There is no directory-service or identity fallback.
 */
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct lookup_state {
    struct passwd record;
    char *buffer;
    size_t capacity;
    struct group group_record;
    char *group_buffer;
    size_t group_capacity;
    FILE *group_stream;
};

struct scan {
    FILE *file;
    char *line;
    size_t capacity;
    int borrowed;
};

static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_key_t lookup_key;
static int key_error;

static void free_state(void *pointer)
{
    struct lookup_state *state = pointer;
    free(state->buffer);
    free(state->group_buffer);
    if (state->group_stream) fclose(state->group_stream);
    free(state);
}

static void make_key(void)
{
    key_error = pthread_key_create(&lookup_key, free_state);
}

static void close_scan(void *pointer)
{
    struct scan *scan = pointer;
    free(scan->line);
    if (scan->file && !scan->borrowed)
        fclose(scan->file);
}

static int number(const char *text, uint64_t maximum, uint64_t *result)
{
    uint64_t value = 0;
    if (!*text)
        return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9')
            return 0;
        unsigned digit = (unsigned)(*text - '0');
        if (value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10))
            return 0;
        value = value * 10 + digit;
    }
    *result = value;
    return 1;
}

static int timestamp(const char *text, time_t *result)
{
    uint64_t value;
    _Static_assert(sizeof(time_t) == 8, "Darwin account timestamps use 64 bits");
    if (!*text) {
        *result = 0;
        return 1;
    }
    int negative = *text == '-';
    if (negative)
        ++text;
    if (!number(text, negative ? (uint64_t)INT64_MAX + 1 : INT64_MAX, &value))
        return 0;
    /* Avoid converting or negating INT64_MAX + 1 as a signed value. */
    *result = negative ? (value ? -(int64_t)(value - 1) - 1 : 0) : (time_t)value;
    return 1;
}

/* Split the actual Darwin master.passwd (10 fields) or passwd (7 fields). */
static int parse(char *line, size_t length, int master, struct passwd *record)
{
    char *field[10];
    unsigned count = 1;
    uint64_t uid, gid;
    if (!length || line[0] == '#' || memchr(line, '\0', length))
        return 0;
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        line[--length] = '\0';
    field[0] = line;
    for (char *p = line; *p; ++p) {
        if (*p == ':') {
            if (count == 10)
                return 0;
            *p = '\0';
            field[count++] = p + 1;
        }
    }
    if (count != (master ? 10U : 7U) || !*field[0] ||
        !number(field[2], (uid_t)-1, &uid) || !number(field[3], (gid_t)-1, &gid))
        return 0;
    memset(record, 0, sizeof(*record));
    record->pw_name = field[0];
    record->pw_passwd = field[1];
    record->pw_uid = (uid_t)uid;
    record->pw_gid = (gid_t)gid;
    record->pw_class = master ? field[4] : "";
    record->pw_gecos = field[master ? 7 : 4];
    record->pw_dir = field[master ? 8 : 5];
    record->pw_shell = field[master ? 9 : 6];
    return !master || (timestamp(field[5], &record->pw_change) &&
                       timestamp(field[6], &record->pw_expire));
}

static int copy_record(const struct passwd *source, struct passwd *destination,
                       char *buffer, size_t length)
{
    const char *strings[] = {source->pw_name, source->pw_passwd, source->pw_class,
                             source->pw_gecos, source->pw_dir, source->pw_shell};
    size_t required = 0;
    for (unsigned i = 0; i < 6; ++i) {
        size_t size = strlen(strings[i]) + 1;
        if (size > SIZE_MAX - required)
            return ERANGE;
        required += size;
    }
    if (length < required)
        return ERANGE;
    *destination = *source;
    char **fields[] = {&destination->pw_name, &destination->pw_passwd, &destination->pw_class,
                       &destination->pw_gecos, &destination->pw_dir, &destination->pw_shell};
    for (unsigned i = 0; i < 6; ++i) {
        size_t size = strlen(strings[i]) + 1;
        memcpy(buffer, strings[i], size);
        *fields[i] = buffer;
        buffer += size;
    }
    return 0;
}

static int lookup(const char *name, uid_t uid, struct passwd *record,
                  char *buffer, size_t length, struct passwd **result)
{
    if (!result)
        return EINVAL;
    *result = NULL;
    if (!record || (!buffer && length))
        return EINVAL;
    int saved_errno = errno;
    int error = 0, master = 1;
    struct scan scan = {0};
    scan.file = fopen("/etc/master.passwd", "r");
    if (!scan.file && (errno == ENOENT || errno == EACCES)) {
        master = 0;
        scan.file = fopen("/etc/passwd", "r");
    }
    if (!scan.file) {
        error = errno ? errno : EIO;
        errno = saved_errno;
        return error;
    }
    pthread_cleanup_push(close_scan, &scan);
    for (;;) {
        errno = 0;
        ssize_t size = getline(&scan.line, &scan.capacity, scan.file);
        if (size < 0) {
            if (ferror(scan.file) || !feof(scan.file))
                error = errno ? errno : EIO;
            break;
        }
        struct passwd candidate;
        if (!parse(scan.line, (size_t)size, master, &candidate) ||
            (name ? strcmp(name, candidate.pw_name) != 0 : candidate.pw_uid != uid))
            continue;
        error = copy_record(&candidate, record, buffer, length);
        if (!error)
            *result = record;
        break;
    }
    pthread_cleanup_pop(1);
    errno = saved_errno;
    return error;
}

int getpwnam_r(const char *name, struct passwd *record, char *buffer, size_t length,
               struct passwd **result)
{
    if (!name) {
        if (result)
            *result = NULL;
        return EINVAL;
    }
    return lookup(name, 0, record, buffer, length, result);
}

int getpwuid_r(uid_t uid, struct passwd *record, char *buffer, size_t length,
               struct passwd **result)
{
    return lookup(NULL, uid, record, buffer, length, result);
}

static struct lookup_state *thread_state(void)
{
    int error = pthread_once(&key_once, make_key);
    if (error || key_error) {
        errno = error ? error : key_error;
        return NULL;
    }
    struct lookup_state *state = pthread_getspecific(lookup_key);
    if (!state) {
        state = calloc(1, sizeof(*state));
        if (!state)
            return NULL;
        error = pthread_setspecific(lookup_key, state);
        if (error) {
            free(state);
            errno = error;
            return NULL;
        }
    }
    return state;
}

static struct passwd *lookup_thread(const char *name, uid_t uid)
{
    struct lookup_state *state = thread_state();
    if (!state) return NULL;
    int error;
    for (;;) {
        if (!state->capacity) {
            state->buffer = malloc(256);
            if (!state->buffer)
                return NULL;
            state->capacity = 256;
        }
        struct passwd *result = NULL;
        error = lookup(name, uid, &state->record, state->buffer, state->capacity, &result);
        if (error != ERANGE) {
            if (error)
                errno = error;
            return result;
        }
        if (state->capacity > SIZE_MAX / 2) {
            errno = ERANGE;
            return NULL;
        }
        char *grown = realloc(state->buffer, state->capacity * 2);
        if (!grown)
            return NULL;
        state->buffer = grown;
        state->capacity *= 2;
    }
}

struct passwd *getpwnam(const char *name)
{
    if (!name) {
        errno = EINVAL;
        return NULL;
    }
    return lookup_thread(name, 0);
}

struct passwd *getpwuid(uid_t uid)
{
    return lookup_thread(NULL, uid);
}

/* The reentrant group API reads actual /etc/group records and places every
 * string and the aligned member vector in the caller's buffer. */
static int lookup_group(const char *name, gid_t gid, struct group *record,
                        char *buffer, size_t length, struct group **result, FILE *enumeration)
{
    if (!result) return EINVAL;
    *result = NULL;
    if (!record || !buffer) return EINVAL;
    int saved_errno = errno, error = 0;
    struct scan scan = {0};
    scan.file = enumeration ? enumeration : fopen("/etc/group", "r");
    scan.borrowed = enumeration != NULL;
    if (!scan.file) {
        error = errno ? errno : EIO;
        errno = saved_errno;
        return error;
    }
    pthread_cleanup_push(close_scan, &scan);
    for (;;) {
        errno = 0;
        ssize_t size = getline(&scan.line, &scan.capacity, scan.file);
        if (size < 0) {
            if (ferror(scan.file) || !feof(scan.file)) error = errno ? errno : EIO;
            break;
        }
        if (!size || scan.line[0] == '#' || memchr(scan.line, '\0', (size_t)size)) continue;
        while (size && (scan.line[size - 1] == '\n' || scan.line[size - 1] == '\r')) scan.line[--size] = 0;
        char *field[4], *cursor = scan.line;
        unsigned count = 0;
        for (; count < 4 && cursor; ++count) field[count] = strsep(&cursor, ":");
        uint64_t candidate_gid;
        if (count != 4 || cursor || !*field[0] || !number(field[2], (gid_t)-1, &candidate_gid) ||
            (!enumeration && (name ? strcmp(field[0], name) != 0 : candidate_gid != gid))) continue;
        size_t members = *field[3] ? 1 : 0;
        int malformed = 0;
        for (char *p = field[3]; *p; ++p) {
            if (*p == ',') {
                ++members;
                if (p == field[3] || !p[1] || p[1] == ',') malformed = 1;
            }
        }
        if (malformed) continue;
        size_t strings = strlen(field[0]) + strlen(field[1]) + strlen(field[3]) + 3;
        size_t padding = (-(uintptr_t)buffer) & (_Alignof(char *) - 1);
        if (members >= SIZE_MAX / sizeof(char *) || padding > length ||
            (members + 1) * sizeof(char *) > length - padding ||
            strings > length - padding - (members + 1) * sizeof(char *)) {
            error = ERANGE;
            break;
        }
        char **vector = (char **)(buffer + padding);
        char *destination = buffer + padding + (members + 1) * sizeof(char *);
        struct group candidate = {0};
        candidate.gr_gid = (gid_t)candidate_gid;
        candidate.gr_mem = vector;
        char **fields[] = {&candidate.gr_name, &candidate.gr_passwd, &cursor};
        for (unsigned i = 0; i < 3; ++i) {
            size_t amount = strlen(field[i == 2 ? 3 : i]) + 1;
            *fields[i] = destination;
            memcpy(destination, field[i == 2 ? 3 : i], amount);
            destination += amount;
        }
        for (size_t i = 0; i < members; ++i) vector[i] = strsep(&cursor, ",");
        vector[members] = NULL;
        *record = candidate;
        *result = record;
        break;
    }
    pthread_cleanup_pop(1);
    errno = saved_errno;
    return error;
}

int getgrgid_r(gid_t gid, struct group *record, char *buffer, size_t length,
               struct group **result)
{
    return lookup_group(NULL, gid, record, buffer, length, result, NULL);
}

int getgrnam_r(const char *name, struct group *record, char *buffer, size_t length,
               struct group **result)
{
    if (!name) {
        if (result) *result = NULL;
        return EINVAL;
    }
    return lookup_group(name, 0, record, buffer, length, result, NULL);
}

/* Traditional group calls use a separate per-thread record and growing
 * buffer. Password lookups cannot invalidate a returned group, or vice versa. */
static struct group *group_thread(const char *name, gid_t gid, FILE *cursor)
{
    struct lookup_state *state = thread_state();
    if (!state) return NULL;
    off_t position = cursor ? ftello(cursor) : 0;
    if (position < 0) return NULL;
    for (;;) {
        if (!state->group_capacity) {
            state->group_buffer = malloc(256);
            if (!state->group_buffer) return NULL;
            state->group_capacity = 256;
        }
        struct group *result = NULL;
        int error = lookup_group(name, gid, &state->group_record, state->group_buffer, state->group_capacity, &result, cursor);
        if (error != ERANGE) {
            if (error) errno = error;
            return result;
        }
        if (state->group_capacity > SIZE_MAX / 2) { errno = ERANGE; return NULL; }
        char *grown = realloc(state->group_buffer, state->group_capacity * 2);
        if (!grown) return NULL;
        state->group_buffer = grown;
        state->group_capacity *= 2;
        if (cursor && fseeko(cursor, position, SEEK_SET)) return NULL;
    }
}

struct group *getgrnam(const char *name)
{
    if (!name) { errno = EINVAL; return NULL; }
    return group_thread(name, 0, NULL);
}
struct group *getgrgid(gid_t gid) { return group_thread(NULL, gid, NULL); }

void setgrent(void)
{
    struct lookup_state *state = thread_state();
    if (!state) return;
    if (state->group_stream) rewind(state->group_stream);
    else state->group_stream = fopen("/etc/group", "r");
}
void endgrent(void)
{
    struct lookup_state *state = thread_state();
    if (!state) return;
    if (state->group_stream) { fclose(state->group_stream); state->group_stream = NULL; }
}
struct group *getgrent(void)
{
    struct lookup_state *state = thread_state();
    if (!state) return NULL;
    if (!state->group_stream) state->group_stream = fopen("/etc/group", "r");
    return state->group_stream ? group_thread(NULL, 0, state->group_stream) : NULL;
}

struct group_list_scan {
    struct scan scan;
    int *groups;
};

static void close_group_list(void *pointer)
{
    struct group_list_scan *state = pointer;
    close_scan(&state->scan);
    free(state->groups);
}

/* Darwin's published API uses int groups and returns zero on success.
 * Include the supplied base group, then actual, unique /etc/group memberships.
 * A short array gets the prefix and the full required count, with return -1. */
int getgrouplist(const char *name, int basegid, int *groups, int *ngroups)
{
    if (!name || !ngroups || *ngroups < 0 || (*ngroups > 0 && !groups)) {
        errno = EINVAL;
        return -1;
    }
    int capacity = *ngroups, saved_errno = errno, error = 0, status = -1;
    size_t count = 1, allocated = 16;
    struct group_list_scan state = {0};
    state.groups = malloc(allocated * sizeof(int));
    if (!state.groups) return -1;
    state.groups[0] = basegid;
    state.scan.file = fopen("/etc/group", "r");
    if (!state.scan.file) {
        error = errno ? errno : EIO;
        free(state.groups);
        errno = error;
        return -1;
    }
    pthread_cleanup_push(close_group_list, &state);
    for (;;) {
        errno = 0;
        ssize_t size = getline(&state.scan.line, &state.scan.capacity, state.scan.file);
        if (size < 0) {
            if (ferror(state.scan.file) || !feof(state.scan.file)) error = errno ? errno : EIO;
            break;
        }
        if (!size || state.scan.line[0] == '#' || memchr(state.scan.line, '\0', (size_t)size)) continue;
        while (size && (state.scan.line[size - 1] == '\n' || state.scan.line[size - 1] == '\r')) state.scan.line[--size] = 0;
        char *field[4], *cursor = state.scan.line;
        unsigned fields = 0;
        for (; fields < 4 && cursor; ++fields) field[fields] = strsep(&cursor, ":");
        uint64_t gid;
        if (fields != 4 || cursor || !*field[0] || !number(field[2], (gid_t)-1, &gid)) continue;
        int member = 0, malformed = 0;
        cursor = field[3];
        while (cursor) {
            char *item = strsep(&cursor, ",");
            if (!*item) malformed = 1;
            if (!strcmp(item, name)) member = 1;
        }
        if (!member || malformed) continue;
        int candidate = (int)(gid_t)gid, duplicate = 0;
        for (size_t i = 0; i < count; ++i)
            if (state.groups[i] == candidate) duplicate = 1;
        if (duplicate) continue;
        if (count == INT_MAX) {
            error = EOVERFLOW;
            break;
        }
        if (count == allocated) {
            size_t amount = allocated * 2;
            if (amount > INT_MAX) amount = INT_MAX;
            int *resized = realloc(state.groups, amount * sizeof(int));
            if (!resized) {
                error = errno ? errno : ENOMEM;
                break;
            }
            state.groups = resized;
            allocated = amount;
        }
        state.groups[count++] = candidate;
    }
    if (!error) {
        size_t filled = count < (size_t)capacity ? count : (size_t)capacity;
        if (filled) memcpy(groups, state.groups, filled * sizeof(int));
        *ngroups = (int)count;
        status = (size_t)capacity >= count ? 0 : -1;
    }
    pthread_cleanup_pop(1);
    errno = error ? error : saved_errno;
    return status;
}

static void free_group_array(void *pointer)
{
    free(*(int **)pointer);
}

int32_t getgroupcount(const char *name, gid_t basegid)
{
    int count = 0;
    (void)getgrouplist(name, (int)basegid, NULL, &count);
    return count;
}

int initgroups(const char *name, int basegid)
{
    int capacity = 16, result = -1, error = 0;
    int *groups = malloc((size_t)capacity * sizeof(int));
    if (!groups) return -1;
    pthread_cleanup_push(free_group_array, &groups);
    for (;;) {
        int count = capacity;
        if (getgrouplist(name, basegid, groups, &count) == 0) {
            result = setgroups(count, (const gid_t *)groups);
            error = errno;
            break;
        }
        if (count <= capacity) {
            error = errno;
            break;
        }
        int *resized = realloc(groups, (size_t)count * sizeof(int));
        if (!resized) {
            error = errno ? errno : ENOMEM;
            break;
        }
        groups = resized;
        capacity = count;
    }
    pthread_cleanup_pop(1);
    errno = error;
    return result;
}
