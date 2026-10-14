#include "native_calls.h"
extern int native_value(int);
extern int native_environment(char *, char *);
extern int absent_native_optional(void) __attribute__((weak_import));
static int (*volatile value)(int) = native_value;
static int (*volatile environment)(char *, char *) = native_environment;
static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static void say(const char *s) { __write_nocancel(1, s, length(s)); }
#ifdef NATIVE_RUNTIME_TEST
extern int native_platform_check(void);
extern int native_mach_probe(void);
static void failure(const char *component, unsigned code) {
    char digits[12]; unsigned n = 0;
    do { digits[n++] = '0' + code % 10; code /= 10; } while (code);
    say("Native runtime failure: "); say(component); say(":");
    while (n) __write_nocancel(1, digits + --n, 1);
    say("\n");
}
#endif
int main(int argc, char **argv, char **envp, char **apple) {
    char release[128] = {0}, machine[128] = {0};
    if (argc != 2 || envp != argv + argc + 1 || !apple || apple <= envp ||
        value(11) != 60 || environment(release, machine) || absent_native_optional) {
        say("Native Mach-O dynamic runtime check failed\n"); return 1;
    }
#ifdef NATIVE_RUNTIME_TEST
    int code = native_mach_probe();
    if (code) { failure("mach", code); return 1; }
    code = native_platform_check();
    if (code) { failure("platform", code); return 1; }
    say("NATIVE_RUNTIME_OK:");
#else
    say("NATIVE_DYLD_OK:");
#endif
    say(argv[1]); say(":"); say(release); say(":"); say(machine); say("\n");
    return 0;
}
