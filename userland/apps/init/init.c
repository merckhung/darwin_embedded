/* Small native embedded PID 1. This is not Apple's launchd service manager. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hfs_mount.h"

extern char **environ;

static void failure(const char *operation)
{
    int saved = errno;
    dprintf(STDERR_FILENO, "Native init failure: %s (errno=%d)\n", operation, saved);
}

static void fatal(const char *operation)
{
    failure(operation);
    /* Exiting PID 1 panics the kernel. Keep the failure visible and wait. */
    for (;;) sleep(60);
}

static void open_console(void)
{
    int console = open("/dev/console", O_RDWR | O_NOCTTY);
    if (console < 0) fatal("open /dev/console");
    for (int fd = 0; fd != 3; ++fd)
        if (dup2(console, fd) < 0) fatal("duplicate console descriptor");
    if (console > STDERR_FILENO && close(console)) fatal("close extra console descriptor");
}

static pid_t start_desktop(void)
{
    FILE *configuration = fopen("/etc/desktop-session", "r");
    if (!configuration) {
        if (errno != ENOENT) failure("read desktop session configuration");
        return 0;
    }
    char executable[256];
    if (!fgets(executable, sizeof(executable), configuration)) {
        fclose(configuration);
        failure("read desktop session executable");
        return 0;
    }
    fclose(configuration);
    size_t length = strlen(executable);
    if (length && executable[length - 1] == '\n') executable[--length] = '\0';
    if (!length || executable[0] != '/' || access(executable, X_OK)) {
        errno = EINVAL;
        failure("invalid desktop session executable");
        return 0;
    }
    pid_t child = fork();
    if (child < 0) { failure("fork desktop supervisor"); return 0; }
    if (!child) {
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            setsid() < 0 || chdir("/var/root")) {
            failure("establish desktop session");
            _exit(126);
        }
        if (input > STDERR_FILENO) close(input);
        char *arguments[] = {executable, NULL};
        execve(arguments[0], arguments, environ);
        failure("execve desktop supervisor");
        _exit(127);
    }
    dprintf(STDOUT_FILENO, "Native init desktop spawned: %d\n", child);
    return child;
}

int main(void)
{
    if (getpid() != 1) {
        errno = EPERM;
        failure("embedded init must be PID 1");
        return 1;
    }
    open_console();

    struct statfs filesystem;
    if (statfs("/", &filesystem)) fatal("inspect root mount");
    if (filesystem.f_flags & MNT_RDONLY) {
        struct hfs_mount_args arguments = {.hfs_mask = 0755};
        if (strcmp(filesystem.f_fstypename, "hfs")) {
            errno = ENOTSUP;
            fatal("read-only root is not HFS");
        }
        if (mount("hfs", "/", MNT_UPDATE, &arguments) || statfs("/", &filesystem))
            fatal("remount HFS memory root writable");
        if (filesystem.f_flags & MNT_RDONLY) {
            errno = EROFS;
            fatal("root remained read-only after remount");
        }
    }
    if (setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1) ||
        setenv("HOME", "/var/root", 1) || setenv("SHELL", "/bin/sh", 1) ||
        setenv("USER", "root", 1) || setenv("LOGNAME", "root", 1) ||
        setenv("TERM", "dumb", 1) || setenv("PS1", "\nroot@Darwin# ", 1))
        fatal("establish shell environment");
    dprintf(STDOUT_FILENO, "Native init: PID 1, writable HFS root, starting /bin/sh\n");
    pid_t desktop = start_desktop();

    for (;;) {
        pid_t shell = fork();
        if (shell < 0) {
            failure("fork shell");
            sleep(1);
            continue;
        }
        if (!shell) {
            if (setsid() < 0 || ioctl(STDIN_FILENO, TIOCSCTTY, 0) < 0 || chdir("/var/root")) {
                failure("establish shell session, controlling console and home");
                _exit(126);
            }
            char *arguments[] = {"/bin/sh", "-i", NULL};
            execve(arguments[0], arguments, environ);
            failure("execve /bin/sh");
            _exit(127);
        }
        dprintf(STDOUT_FILENO, "Native init spawned: %d\n", shell);
        for (;;) {
            int status;
            pid_t child = waitpid(-1, &status, 0);
            if (child < 0) {
                if (errno == EINTR) continue;
                failure("waitpid child");
                sleep(1);
                continue;
            }
            if (child == desktop) {
                desktop = 0;
                dprintf(STDOUT_FILENO, "Native init desktop reaped: %d:%d\n", child,
                        WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
            }
            if (child != shell) continue;
            /* Session-leader exit revokes Darwin's controlling tty handles.
             * Reopen the console before reporting status or spawning again. */
            if (WIFEXITED(status) || WIFSIGNALED(status)) open_console();
            if (WIFEXITED(status))
                dprintf(STDOUT_FILENO, "Native init reaped: %d:%d\n", child, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                dprintf(STDOUT_FILENO, "Native init: reaped shell pid=%d signal=%d\n", child, WTERMSIG(status));
            else continue;
            break;
        }
        sleep(1);
    }
}
