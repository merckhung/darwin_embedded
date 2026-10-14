/* Guest proof of actual HFS ownership and unprivileged home-file access. */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failure(const char *operation)
{
    dprintf(STDERR_FILENO, "Native shell failure: desktop home %s (errno=%d)\n", operation, errno);
    return 1;
}

static int child_check(const char *nonce)
{
    if (setgroups(0, NULL) || setgid(20) || setuid(501)) return failure("drop groups and identity");
    if (getuid() != 501 || geteuid() != 501 || getgid() != 20 || getegid() != 20)
        return failure("real and effective identity mismatch");
    if (chdir("/home/desktop")) return failure("enter owned directory");
    char path[96];
    if (snprintf(path, sizeof(path), ".native-home-%s", nonce) >= (int)sizeof(path))
        return failure("nonce filename too long");
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return failure("create nonce file as UID 501");
    int result = 0;
    struct stat status;
    char data[49];
    if (write(fd, nonce, 48) != 48 || fstat(fd, &status) ||
        status.st_uid != 501 || status.st_gid != 20 || (status.st_mode & 07777) != 0600 ||
        lseek(fd, 0, SEEK_SET) != 0 || read(fd, data, 48) != 48 || memcmp(data, nonce, 48) ||
        read(fd, data, 1) != 0)
        result = failure("write, metadata or readback");
    if (close(fd)) result = failure("close nonce file");
    if (unlink(path)) result = failure("unlink nonce file as UID 501");
    errno = 0;
    if (stat(path, &status) == 0 || errno != ENOENT) result = failure("file still exists after unlink");
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) != 48) return failure("fresh nonce required");
    for (unsigned n = 0; n != 48; ++n)
        if (!((argv[1][n] >= '0' && argv[1][n] <= '9') ||
              (argv[1][n] >= 'a' && argv[1][n] <= 'f')))
            return failure("invalid nonce");
    struct stat status;
    if (stat("/home/desktop", &status) || !S_ISDIR(status.st_mode) ||
        status.st_uid != 501 || status.st_gid != 20 || (status.st_mode & 07777) != 0700)
        return failure("directory must be UID 501 GID 20 mode 0700");
    pid_t child = fork();
    if (child < 0) return failure("fork unprivileged child");
    if (!child) _exit(child_check(argv[1]));
    pid_t waited;
    int child_status;
    do { waited = waitpid(child, &child_status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(child_status) || WEXITSTATUS(child_status))
        return failure("unprivileged child did not exit successfully");
    printf("NATIVE_HOME_OK:%s:501:20\n", argv[1]);
    return 0;
}
