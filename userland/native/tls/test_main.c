#include <unistd.h>
#include <stdlib.h>
#include <sys/sysctl.h>

extern int native_tls_check(void);
extern int native_system_check(void);

static int say(const char *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    while (length) {
        ssize_t count = write(1, text, length);
        if (count <= 0) return 1;
        text += count;
        length -= count;
    }
    return 0;
}

static int fail(unsigned code)
{
    char digits[12];
    unsigned count = 0;
    do { digits[count++] = '0' + code % 10; code /= 10; } while (code);
    say("Native TLS failure: ");
    while (count) write(1, digits + --count, 1);
    say("\n");
    return 1;
}

int main(int argc, char **argv, char **envp, char **apple)
{
    char release[128], machine[128];
    if (argc != 2 || envp != argv + argc + 1 || !apple || apple <= envp)
        return fail(100);
    int mib[2] = {CTL_KERN, KERN_OSRELEASE};
    size_t size = sizeof(release);
    if (sysctl(mib, 2, release, &size, 0, 0) || !size ||
        size > sizeof(release) || release[size - 1]) return fail(101);
    mib[0] = CTL_HW; mib[1] = HW_MACHINE; size = sizeof(machine);
    if (sysctl(mib, 2, machine, &size, 0, 0) || !size ||
        size > sizeof(machine) || machine[size - 1]) return fail(102);
    int code = native_tls_check();
    if (code) return fail((unsigned)code);
    code = native_system_check();
    if (code) return fail(1000 + (unsigned)code);
    if (say("NATIVE_TLS_OK:") || say(argv[1]) || say(":") || say(release) ||
        say(":") || say(machine) || say("\n"))
        return 1;
    exit(0);
}
