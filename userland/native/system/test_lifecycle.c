#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <crt_externs.h>

extern char **environ;
static unsigned order;
static void prepare_a(void) { order = order * 10 + 1; }
static void prepare_b(void) { order = order * 10 + 2; }
static void parent_a(void) { order = order * 10 + 3; }
static void parent_b(void) { order = order * 10 + 4; }
static void child_a(void) { order = order * 10 + 5; }
static void child_b(void) { order = order * 10 + 6; }

struct reader {
    pthread_mutex_t lock;
    pthread_cond_t ready_condition;
    int ready;
    int fd;
};

static void *cancel_read(void *opaque)
{
    struct reader *reader = opaque;
    if (pthread_mutex_lock(&reader->lock)) return (void *)1;
    reader->ready = 1;
    if (pthread_cond_signal(&reader->ready_condition)) return (void *)2;
    if (pthread_mutex_unlock(&reader->lock)) return (void *)3;
    char byte;
    /* A real native cancellation point, including a pending cancellation
       delivered before the read enters the kernel. The pipe stays empty. */
    (void)read(reader->fd, &byte, 1);
    return (void *)4;
}

int native_system_check(void)
{
    if (!pthread_main_np() || !pthread_self()) return 1;
    jmp_buf context;
    volatile unsigned crossed = 0;
    int jumped = setjmp(context);
    if (!jumped) {
        crossed = 1;
        longjmp(context, 7);
    }
    if (jumped != 7 || crossed != 1) return 28;
    sigset_t block, old, current;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block, &old)) return 29;
    sigjmp_buf signal_context;
    jumped = sigsetjmp(signal_context, 1);
    if (!jumped) {
        if (sigprocmask(SIG_UNBLOCK, &block, 0)) return 30;
        siglongjmp(signal_context, 9);
    }
    if (jumped != 9 || sigprocmask(SIG_BLOCK, 0, &current) ||
        sigismember(&current, SIGUSR1) != 1) return 31;
    if (sigprocmask(SIG_SETMASK, &old, 0)) return 32;
    if (!environ || environ != *_NSGetEnviron()) return 2;
    if (setenv("NATIVE_SYSTEM_LIFECYCLE", "parent", 1)) return 3;
    if (environ != *_NSGetEnviron() || strcmp(getenv("NATIVE_SYSTEM_LIFECYCLE"), "parent")) return 4;
    char *allocation = malloc(8193);
    if (!allocation) return 5;
    memset(allocation, 0x6d, 8193);
    if (pthread_atfork(prepare_a, parent_a, child_a) ||
        pthread_atfork(prepare_b, parent_b, child_b)) return 6;
    int pipefd[2];
    if (pipe(pipefd)) return 7;
    pid_t child = fork();
    if (child < 0) return 8;
    if (!child) {
        close(pipefd[0]);
        unsigned result = 0;
        if (order != 2156) result = 9;
        if (!pthread_main_np() || !pthread_self()) result = 10;
        if (allocation[0] != 0x6d || allocation[8192] != 0x6d) result = 11;
        char *changed = realloc(allocation, 16387);
        if (!changed) result = 12;
        else {
            if (changed[0] != 0x6d || changed[8192] != 0x6d) result = 13;
            memset(changed, 0x2a, 16387);
            free(changed);
        }
        if (setenv("NATIVE_SYSTEM_LIFECYCLE", "child", 1) ||
            environ != *_NSGetEnviron() || strcmp(getenv("NATIVE_SYSTEM_LIFECYCLE"), "child")) result = 14;
        ssize_t count;
        do { count = write(pipefd[1], &result, sizeof(result)); } while (count < 0 && errno == EINTR);
        _exit(count == sizeof(result) ? 0 : 15);
    }
    close(pipefd[1]);
    if (order != 2134) return 16;
    unsigned result = 99;
    ssize_t count;
    do { count = read(pipefd[0], &result, sizeof(result)); } while (count < 0 && errno == EINTR);
    close(pipefd[0]);
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) || count != sizeof(result)) return 17;
    if (result) return result;
    if (allocation[0] != 0x6d || allocation[8192] != 0x6d ||
        strcmp(getenv("NATIVE_SYSTEM_LIFECYCLE"), "parent")) return 18;
    free(allocation);
    if (unsetenv("NATIVE_SYSTEM_LIFECYCLE") || getenv("NATIVE_SYSTEM_LIFECYCLE")) return 19;

    if (pipe(pipefd)) return 20;
    struct reader reader = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, pipefd[0]};
    pthread_t thread;
    if (pthread_mutex_lock(&reader.lock)) return 21;
    if (pthread_create(&thread, 0, cancel_read, &reader)) return 22;
    while (!reader.ready)
        if (pthread_cond_wait(&reader.ready_condition, &reader.lock)) return 23;
    if (pthread_mutex_unlock(&reader.lock)) return 24;
    if (pthread_cancel(thread)) return 25;
    void *value;
    if (pthread_join(thread, &value) || value != PTHREAD_CANCELED) return 26;
    close(pipefd[0]); close(pipefd[1]);
    if (pthread_cond_destroy(&reader.ready_condition) || pthread_mutex_destroy(&reader.lock)) return 27;
    return 0;
}
