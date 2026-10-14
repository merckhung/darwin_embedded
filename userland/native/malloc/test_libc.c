/* Exercise the public C library after its actual native initializer. */
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int native_libc_check(void)
{
    if (!setlocale(LC_ALL, "C")) return 110;
    char text[96];
    const char *expected = "00001a2b -1234567890123 3.125";
    int count = snprintf(text, sizeof(text), "%08x %lld %.3f", 0x1a2b,
                         -1234567890123LL, 3.125);
    if (count != (int)strlen(expected) || strcmp(text, expected)) return 111;
    char small[5];
    if (snprintf(small, sizeof(small), "%s", "abcdef") != 6 || strcmp(small, "abcd"))
        return 112;
    char *end;
    if (strtod("  -3.125tail", &end) != -3.125 || strcmp(end, "tail")) return 113;
    if (setenv("NATIVE_LIBC_CHECK", "first", 1) ||
            strcmp(getenv("NATIVE_LIBC_CHECK"), "first")) return 114;
    if (setenv("NATIVE_LIBC_CHECK", "replacement", 1) ||
            strcmp(getenv("NATIVE_LIBC_CHECK"), "replacement")) return 115;
    if (unsetenv("NATIVE_LIBC_CHECK") || getenv("NATIVE_LIBC_CHECK")) return 116;
    if (setenv("TZ", "UTC0", 1)) return 117;
    tzset();
    time_t epoch = 946684800;
    struct tm local, utc;
    if (!localtime_r(&epoch, &local) || !gmtime_r(&epoch, &utc) ||
            local.tm_year != 100 || local.tm_mon != 0 || local.tm_mday != 1 ||
            local.tm_hour || local.tm_min || local.tm_sec || local.tm_wday != 6 ||
            local.tm_yday || local.tm_isdst || utc.tm_hour || utc.tm_mday != 1)
        return 118;
    if (strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %Z", &local) != 23 ||
            strcmp(text, "2000-01-01 00:00:00 UTC")) return 119;
    if (mktime(&local) != epoch) return 120;
    if (arc4random_uniform(0) || arc4random_uniform(1)) return 121;
    unsigned char random_first[33], random_second[33];
    arc4random_buf(random_first, sizeof(random_first));
    arc4random_buf(random_second, sizeof(random_second));
    if (!memcmp(random_first, random_second, sizeof(random_first))) return 122;
    for (unsigned n = 0; n < 16; ++n)
        if (arc4random_uniform(17) >= 17) return 123;
    return 0;
}
