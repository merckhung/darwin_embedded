#include "native_calls.h"
extern int native_double(int);
static int (*volatile leaf)(int) = native_double;
int native_value(int value) { return leaf(value) + 7; }
int native_environment(char *release, char *machine) {
    int mib[2] = {1, 2}; size_t size = 128;
    if (__sysctl(mib, 2, release, &size, 0, 0)) return 1;
    mib[0] = 6; mib[1] = 1; size = 128;
    if (__sysctl(mib, 2, machine, &size, 0, 0)) return 2;
    if (__getpid() <= 1) return 3;
    if (__write_nocancel(-1, "invalid", 7) != -1 || *__error() != 9) return 4;
    return 0;
}
