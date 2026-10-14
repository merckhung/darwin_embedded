#include "native_calls.h"

extern int native_math_probe(void);

static int say(const char *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    while (length) {
        long count = __write_nocancel(1, text, length);
        if (count <= 0) return 1;
        text += count;
        length -= (size_t)count;
    }
    return 0;
}

static int fail(unsigned code)
{
    char digits[12];
    unsigned count = 0;
    do { digits[count++] = '0' + code % 10; code /= 10; } while (code);
    say("Native math failure: ");
    while (count) __write_nocancel(1, digits + --count, 1);
    say("\n");
    return 1;
}

int main(int argc, char **argv, char **envp, char **apple)
{
    char release[128], machine[128];
    if (argc != 2 || envp != argv + argc + 1 || !apple || apple <= envp)
        return fail(100);
    int mib[2] = {1, 2};
    size_t size = sizeof(release);
    if (__sysctl(mib, 2, release, &size, 0, 0) || !size ||
        size > sizeof(release) || release[size - 1]) return fail(101);
    mib[0] = 6; mib[1] = 1; size = sizeof(machine);
    if (__sysctl(mib, 2, machine, &size, 0, 0) || !size ||
        size > sizeof(machine) || machine[size - 1]) return fail(102);
    int code = native_math_probe();
    if (code) return fail((unsigned)code);
    return say("NATIVE_MATH_OK:") || say(argv[1]) || say(":") || say(release) ||
        say(":") || say(machine) || say("\n");
}
