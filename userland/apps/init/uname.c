/* Native uname utility using the genuine Darwin libc uname implementation. */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    struct utsname name;
    if (uname(&name)) { perror("uname"); return 1; }
    unsigned fields = 0;
    for (int n = 1; n < argc; ++n) {
        if (argv[n][0] != '-' || !argv[n][1]) goto usage;
        for (const char *flag = argv[n] + 1; *flag; ++flag) {
            switch (*flag) {
            case 'a': fields = 31; break;
            case 's': fields |= 1; break;
            case 'n': fields |= 2; break;
            case 'r': fields |= 4; break;
            case 'v': fields |= 8; break;
            case 'm': fields |= 16; break;
            default: goto usage;
            }
        }
    }
    if (!fields) fields = 1;
    const char *values[] = {name.sysname, name.nodename, name.release, name.version, name.machine};
    const char *separator = "";
    for (unsigned n = 0; n != 5; ++n)
        if (fields & (1u << n)) {
            if (printf("%s%s", separator, values[n]) < 0) return 1;
            separator = " ";
        }
    return putchar('\n') == EOF;
usage:
    fputs("usage: uname [-asnrvm]\n", stderr);
    return 2;
}
