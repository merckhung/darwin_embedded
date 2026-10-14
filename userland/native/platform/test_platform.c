/* Native guest checks for the source-built Darwin libplatform primitives. */
#define PRIVATE 1
#define OSATOMIC_USE_INLINED 0
#define OSATOMIC_DEPRECATED 0
#include <errno.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_traps.h>
#include <os/lock.h>
#include <os/once_private.h>
#include <os/os_sync_wait_on_address.h>
#include <os/tsd.h>
#include <stdint.h>
#include <string.h>

static void once_increment(void *context)
{
    unsigned *calls = context;
    *calls += 1;
}

/* Return the failing check number, or zero after all checks pass. */
int native_platform_check(void)
{
    struct {
        uint64_t before;
        char bytes[64];
        uint64_t after;
    } buffer = {0xabcdef0123456789ULL, {0}, 0x9876543210fedcbaULL};
    static const unsigned char high_bytes[] = {0x01, 0x7f, 0x80, 0xfe, 0xff, 0};

    memset(buffer.bytes, 0x5a, sizeof(buffer.bytes));
    memcpy(buffer.bytes + 3, "0123456789abcdef", 17);
    memmove(buffer.bytes + 5, buffer.bytes + 3, 17);
    if (memcmp(buffer.bytes + 5, "0123456789abcdef", 17)) return 1;
    memmove(buffer.bytes + 1, buffer.bytes + 5, 17);
    if (memcmp(buffer.bytes + 1, "0123456789abcdef", 17)) return 2;
    if (buffer.bytes[0] != 0x5a || buffer.bytes[63] != 0x5a) return 3;
    if (buffer.before != 0xabcdef0123456789ULL ||
        buffer.after != 0x9876543210fedcbaULL) return 4;
    if (memchr(high_bytes, 0xfe, sizeof(high_bytes)) != high_bytes + 3 ||
        memchr(high_bytes, 0x42, sizeof(high_bytes))) return 5;
    if (memcmp("\x80", "\x7f", 1) <= 0 ||
        memcmp("\x7f", "\x80", 1) >= 0) return 6;

    if (strlen(buffer.bytes + 1) != 16 ||
        strnlen(buffer.bytes + 1, 7) != 7 || strlen("") != 0) return 7;
    if (strlcpy(buffer.bytes + 1, "Darwin native platform", 8) != 22 ||
        strcmp(buffer.bytes + 1, "Darwin ")) return 8;
    if (strlcat(buffer.bytes + 1, "Mach-O", 16) != 13 ||
        strcmp(buffer.bytes + 1, "Darwin Mach-O")) return 9;
    if (strstr(buffer.bytes + 1, "Mach-O") != buffer.bytes + 8 ||
        strstr(buffer.bytes + 1, "Linux") ||
        strncmp("abcd", "abce", 3) || strncmp("abcd", "abce", 4) >= 0) return 10;
    memset(buffer.bytes, 0x7f, sizeof(buffer.bytes));
    strncpy(buffer.bytes + 1, "abc", 8);
    if (memcmp(buffer.bytes + 1, "abc\0\0\0\0\0", 8) ||
        buffer.bytes[0] != 0x7f || buffer.bytes[9] != 0x7f) return 11;

    volatile int32_t atomic32 = 10;
    volatile int64_t atomic64 = 0x1234567800000001LL;
    if (OSAtomicAdd32(7, &atomic32) != 17 || atomic32 != 17) return 12;
    if (OSAtomicCompareAndSwap32(16, 40, &atomic32) || atomic32 != 17) return 13;
    if (!OSAtomicCompareAndSwap32Barrier(17, 40, &atomic32) || atomic32 != 40) return 14;
    if (!OSAtomicCompareAndSwap64(0x1234567800000001LL, 0x7abcdef012345678LL,
                                 &atomic64) || atomic64 != 0x7abcdef012345678LL) return 15;
    if (OSAtomicAdd64Barrier(-8, &atomic64) != 0x7abcdef012345670LL) return 16;

    /* The standalone runtime test has a native TSD base but no pthread yet.
     * Obtain the real send right from XNU and use Apple's TSD slot definition.
     * Its lifetime is the test process's lifetime, when XNU releases the right.
     */
    if (!_os_tsd_get_direct(__TSD_MACH_THREAD_SELF)) {
        mach_port_name_t owner = thread_self_trap();
        if (!MACH_PORT_VALID(owner)) return 17;
        _os_tsd_set_direct(__TSD_MACH_THREAD_SELF, (void *)(uintptr_t)owner);
    }
    os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    if (!os_unfair_lock_trylock(&lock)) return 18;
    if (os_unfair_lock_trylock(&lock)) return 19;
    os_unfair_lock_unlock(&lock);
    os_unfair_lock_lock(&lock);
    if (os_unfair_lock_trylock(&lock)) return 20;
    os_unfair_lock_unlock(&lock);

    os_once_t once = 0;
    unsigned calls = 0;
    _os_once(&once, &calls, once_increment);
    _os_once(&once, &calls, once_increment);
    if (calls != 1) return 21;

    uint32_t value = 0x11223344;
    if (os_sync_wait_on_address_with_timeout(&value, value + 1, sizeof(value),
            OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, 1000000) != 0) return 22;
    errno = 0;
    if (os_sync_wait_on_address_with_timeout(&value, value, sizeof(value),
            OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, 1000000) != -1 ||
        errno != ETIMEDOUT) return 23;
    return 0;
}
