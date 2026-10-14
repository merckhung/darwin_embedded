/* Executed in XNU before the real upstream shell runs the script checks. */
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>
#include "hfs_mount.h"

static int fail(const char *message)
{
    fprintf(stderr, "Native shell failure: %s (errno=%d)\n", message, errno);
    return 1;
}

struct worker {
    const char *name;
    uid_t uid;
    struct passwd *record;
    int error;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned ready;
static int release;

static void *lookup_worker(void *pointer)
{
    struct worker *worker = pointer;
    worker->record = getpwnam(worker->name);
    pthread_mutex_lock(&lock);
    ++ready;
    pthread_cond_broadcast(&condition);
    while (!release)
        pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    if (!worker->record || strcmp(worker->record->pw_name, worker->name) ||
        worker->record->pw_uid != worker->uid)
        worker->error = 1;
    return NULL;
}

static int check_accounts(void)
{
    struct passwd *record = getpwnam("root");
    if (!record || record->pw_uid != 0 || record->pw_gid != 0 ||
        strcmp(record->pw_dir, "/var/root") || strcmp(record->pw_shell, "/bin/dash"))
        return fail("root account must match the installed master.passwd record");
    record = getpwuid(501);
    if (!record || strcmp(record->pw_name, "desktop") || record->pw_gid != 20 ||
        strcmp(record->pw_class, "staff") || record->pw_change != -1 ||
        strcmp(record->pw_gecos, "Desktop User"))
        return fail("UID lookup must preserve the Darwin account fields");
    record = getpwnam("longrecord");
    if (!record || strlen(record->pw_gecos) != 600)
        return fail("thread lookup must grow storage for actual large records");
    if (getpwnam("missing-account") || getpwnam("overflow") || getpwnam("badtime"))
        return fail("missing and malformed records must not invent accounts");
    struct passwd caller, *result = (struct passwd *)1;
    char buffer[1024];
    errno = EDOM;
    if (getpwnam_r("missing-account", &caller, buffer, sizeof(buffer), &result) ||
        result || errno != EDOM)
        return fail("reentrant missing lookup must return zero and NULL without changing errno");
    if (getpwuid_r(501, &caller, buffer, 1, &result) != ERANGE || result)
        return fail("short caller storage must return ERANGE and NULL");
    if (getpwuid_r(501, &caller, buffer, sizeof(buffer), &result) || result != &caller ||
        strcmp(caller.pw_name, "desktop") || caller.pw_uid != 501)
        return fail("reentrant UID lookup must use the caller's record and buffer");
    const char *fields[] = {caller.pw_name, caller.pw_passwd, caller.pw_class,
                           caller.pw_gecos, caller.pw_dir, caller.pw_shell};
    for (unsigned i = 0; i < 6; ++i)
        if ((uintptr_t)fields[i] < (uintptr_t)buffer ||
            (uintptr_t)fields[i] >= (uintptr_t)buffer + sizeof(buffer))
            return fail("reentrant strings must reside in caller storage");

    pthread_t threads[2];
    struct worker workers[2] = {{"root", 0, NULL, 0}, {"desktop", 501, NULL, 0}};
    if (pthread_create(&threads[0], NULL, lookup_worker, &workers[0]) ||
        pthread_create(&threads[1], NULL, lookup_worker, &workers[1]))
        return fail("native account lookup worker creation");
    pthread_mutex_lock(&lock);
    while (ready != 2)
        pthread_cond_wait(&condition, &lock);
    record = getpwnam("longrecord");
    int independent = record && workers[0].record && workers[1].record &&
                      record != workers[0].record && record != workers[1].record &&
                      workers[0].record != workers[1].record;
    release = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    if (pthread_join(threads[0], NULL) || pthread_join(threads[1], NULL) ||
        !independent || workers[0].error || workers[1].error)
        return fail("simultaneous threads must retain independent account records");

    if (rename("/etc/master.passwd", "/etc/master.passwd.saved"))
        return fail("prepare real passwd fallback");
    record = getpwuid(501);
    int fallback = record && !strcmp(record->pw_name, "desktop") &&
                   !strcmp(record->pw_passwd, "*") && !strcmp(record->pw_class, "") &&
                   record->pw_change == 0 && record->pw_expire == 0;
    if (rename("/etc/passwd", "/etc/passwd.saved")) {
        rename("/etc/master.passwd.saved", "/etc/master.passwd");
        return fail("prepare actual missing account files");
    }
    int missing = getpwuid_r(0, &caller, buffer, sizeof(buffer), &result);
    int restored = rename("/etc/passwd.saved", "/etc/passwd") == 0;
    restored &= rename("/etc/master.passwd.saved", "/etc/master.passwd") == 0;
    if (!fallback || missing != ENOENT || result || !restored)
        return fail("fallback, missing file error, or account file restoration");
    return 0;
}

int main(int argc, char **argv, char **environment)
{
    if (argc == 3 && !strcmp(argv[1], "child")) {
        const char *value = getenv("NATIVE_SHELL_ENV");
        if (!value || strcmp(value, argv[2]))
            return fail("fork and exec must preserve the real exported environment");
        printf("exec:%s\n", argv[2]);
        return 0;
    }
    if (argc != 2 || strlen(argv[1]) != 48)
        return fail("fresh guest nonce required");
    struct statfs filesystem;
    if (statfs("/", &filesystem))
        return fail("inspect actual root mount flags");
    if (filesystem.f_flags & MNT_RDONLY) {
        struct hfs_mount_args arguments = {.hfs_mask = 0755};
        if (strcmp(filesystem.f_fstypename, "hfs") ||
            mount("hfs", "/", MNT_UPDATE, &arguments) || statfs("/", &filesystem) ||
            (filesystem.f_flags & MNT_RDONLY))
            return fail("remount actual HFS memory root writable through Darwin mount");
    }
    if (check_accounts())
        return 1;
    printf("Native accounts verified: file records, caller storage, thread isolation and real errors\n");
    fflush(stdout);
    char *arguments[] = {"/bin/dash", "/etc/native-shell-test.sh", argv[1], NULL};
    execve(arguments[0], arguments, environment);
    return fail("execve of actual Mach-O dash");
}
