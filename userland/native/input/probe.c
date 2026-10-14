/* Exercise actual IOKit input with nonce-bound host hardware events. */
#include "NativeBootInput.h"
#include <NativeIOKit.h>
#include <NativeIOKitMIG.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <grp.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "NATIVE_INPUT_FAIL:%s\n", label); return 1; } } while (0)

static int find_service(io_service_t *result)
{
    mach_port_t host = mach_host_self(), main_port = MACH_PORT_NULL;
    CHECK(host_get_io_main(host, &main_port) == KERN_SUCCESS && main_port, "main-port");
    CHECK(mach_port_deallocate(mach_task_self(), host) == KERN_SUCCESS, "release-host");
    io_iterator_t iterator = MACH_PORT_NULL;
    io_string_t match = "<dict><key>IOProviderClass</key><string>NativeBootInput</string></dict>";
    kern_return_t matching = KERN_FAILURE;
    CHECK(io_service_get_matching_services_ool(main_port, match, (mach_msg_type_number_t)strlen(match) + 1,
        &matching, &iterator) == KERN_SUCCESS && matching == KERN_SUCCESS, "matching");
    CHECK(mach_port_deallocate(mach_task_self(), main_port) == KERN_SUCCESS, "release-main");
    *result = IOIteratorNext(iterator);
    CHECK(*result && !IOIteratorNext(iterator), "unique-service");
    CHECK(IOObjectRelease(iterator) == KERN_SUCCESS, "release-iterator");
    return 0;
}

static int check_event(io_connect_t connection, uint64_t *sequence, const NativeInputEvent *expected, const char *nonce)
{
    mach_timebase_info_data_t clock;
    CHECK(mach_timebase_info(&clock) == KERN_SUCCESS && clock.numer && clock.denom, "clock");
    uint64_t start = mach_absolute_time();
    for (;;) {
        NativeInputBatch batch;
        size_t size = sizeof(batch);
        CHECK(IOConnectCallStructMethod(connection, kNativeInputRead, NULL, 0, &batch, &size) == KERN_SUCCESS &&
            size == sizeof(batch) && batch.version == kNativeInputVersion && batch.count <= kNativeInputBatchSize && !batch.dropped, "read-batch");
        if (batch.count) {
            CHECK(batch.count == 1, "unexpected-event-batch");
            const NativeInputEvent *event = &batch.events[0];
            printf("NATIVE_INPUT_EVENT:%s:%llu:%u:%u:%d:%d\n", nonce,
                (unsigned long long)event->sequence, event->type, event->code, event->value, event->value2);
            CHECK(event->sequence > *sequence && event->type == expected->type && event->code == expected->code &&
                event->value == expected->value && event->value2 == expected->value2, "hardware-event-mismatch");
            *sequence = event->sequence;
            return 0;
        }
        uint64_t ticks = mach_absolute_time() - start;
        CHECK((__uint128_t)ticks * clock.numer < (__uint128_t)UINT64_C(20000000000) * clock.denom, "hardware-event-timeout");
        CHECK(usleep(5000) == 0, "poll-delay");
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && strlen(argv[1]) == 48, "nonce");
    struct utsname identity;
    CHECK(uname(&identity) == 0, "uname");
    io_service_t service;
    CHECK(find_service(&service) == 0, "discover-service");
    io_connect_t connection = 0, other = 0;
    CHECK(IOServiceOpen(service, mach_task_self(), 9, &other) == kIOReturnBadArgument && !other, "bad-client-type");
    CHECK(IOServiceOpen(service, mach_task_self(), kNativeInputConnection, &connection) == KERN_SUCCESS, "open");
    CHECK(IOServiceOpen(service, mach_task_self(), kNativeInputConnection, &other) == kIOReturnExclusiveAccess && !other, "exclusive");
    pid_t child = fork();
    CHECK(child >= 0, "fork");
    if (!child) {
        if (setgroups(0, NULL) || setgid(20) || setuid(501)) _exit(10);
        io_service_t own_service;
        if (find_service(&own_service)) _exit(11);
        io_connect_t denied = 0;
        kern_return_t result = IOServiceOpen(own_service, mach_task_self(), kNativeInputConnection, &denied);
        IOObjectRelease(own_service);
        _exit(result == kIOReturnNotPrivileged && !denied ? 0 : 12);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    CHECK(waited == child && WIFEXITED(status) && !WEXITSTATUS(status), "privilege");
    NativeInputInfo info;
    size_t size = sizeof(info);
    CHECK(IOConnectCallStructMethod(connection, 9, NULL, 0, &info, &size) == kIOReturnUnsupported, "bad-selector");
    size = sizeof(info) - 1;
    CHECK(IOConnectCallStructMethod(connection, kNativeInputInformation, NULL, 0, &info, &size) == kIOReturnBadArgument, "bad-info-size");
    size = sizeof(info);
    CHECK(IOConnectCallStructMethod(connection, kNativeInputInformation, NULL, 0, &info, &size) == KERN_SUCCESS &&
        size == sizeof(info) && info.version == 1 && info.capabilities == 6 && info.capacity == kNativeInputQueueSize && !info.dropped, "info");
    NativeInputBatch batch;
    size = sizeof(batch) - 1;
    CHECK(IOConnectCallStructMethod(connection, kNativeInputRead, NULL, 0, &batch, &size) == kIOReturnBadArgument, "bad-read-size");
    uint64_t sequence = 0;
    const NativeInputEvent expected[] = {
        {0,kNativeInputKeyboard,4,1,0}, {0,kNativeInputKeyboard,4,0,0},
        {0,kNativeInputKeyboard,80,1,0}, {0,kNativeInputKeyboard,80,0,0},
        {0,kNativeInputPointer,0,37,13}, {0,kNativeInputPointer,1,0,0}, {0,kNativeInputPointer,0,0,0},
        {0,kNativeInputKeyboard,225,1,0}, {0,kNativeInputKeyboard,4,1,0},
        {0,kNativeInputKeyboard,4,0,0}, {0,kNativeInputKeyboard,225,0,0},
    };
    for (unsigned i = 0; i < sizeof(expected)/sizeof(*expected); ++i) {
        printf("NATIVE_INPUT_READY:%s:%s:%s:%u\n", argv[1], identity.release, identity.machine, i);
        CHECK(fflush(stdout) == 0, "flush");
        CHECK(check_event(connection, &sequence, &expected[i], argv[1]) == 0, "event");
    }
    CHECK(IOServiceClose(connection) == KERN_SUCCESS, "close");
    CHECK(IOServiceOpen(service, mach_task_self(), kNativeInputConnection, &other) == KERN_SUCCESS, "reopen");
    size = sizeof(batch);
    CHECK(IOConnectCallStructMethod(other, kNativeInputRead, NULL, 0, &batch, &size) == KERN_SUCCESS &&
        batch.version == 1 && !batch.count && !batch.dropped, "empty-reopened-queue");
    CHECK(IOServiceClose(other) == KERN_SUCCESS && IOObjectRelease(service) == KERN_SUCCESS, "cleanup");
    printf("NATIVE_INPUT_OK:%s:%s:%s:events=%u\n", argv[1], identity.release, identity.machine,
        (unsigned)(sizeof(expected)/sizeof(*expected)));
    return 0;
}
