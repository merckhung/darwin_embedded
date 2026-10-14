/* Native IOKit input: PC i8042 and BCM2711 DWC2 boot HID backends. */
#include "NativeBootInput.h"
#include "ps2_decode.h"
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSBoolean.h>

#if defined(__arm64__)
#include "NativeDWC2.h"
#else
static uint8_t port_read(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
static void port_write(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port) : "memory");
}
static bool send_byte(uint16_t port, uint8_t value)
{
    for (unsigned retry = 0; retry < 10000; ++retry) {
        if (!(port_read(0x64) & 2)) { port_write(port, value); return true; }
        IODelay(10);
    }
    return false;
}
static bool receive_byte(bool mouse, uint8_t *value)
{
    for (unsigned retry = 0; retry < 10000; ++retry) {
        uint8_t status = port_read(0x64);
        if (status & 1) {
            uint8_t byte = port_read(0x60);
            if (!(status & 0xc0) && !!(status & 0x20) == mouse) { *value = byte; return true; }
        }
        IODelay(10);
    }
    return false;
}
static bool device_command(bool mouse, uint8_t command)
{
    uint8_t ack;
    return (!mouse || send_byte(0x64, 0xd4)) && send_byte(0x60, command) &&
        receive_byte(mouse, &ack) && ack == 0xfa;
}
#endif

class NativeBootInput : public IOService {
    OSDeclareDefaultStructors(NativeBootInput);
    IOWorkLoop *loop;
    IOTimerEventSource *timer;
    IOLock *queueLock;
    NativePS2Decoder decoder;
    NativeInputEvent queue[kNativeInputQueueSize];
    uint32_t head, count;
    uint64_t sequence, dropped;
    bool running;
#if defined(__arm64__)
    NativeDWC2 *usb;
#endif
    static void poll(OSObject *owner, IOTimerEventSource *source);
    static IOReturn stopPolling(OSObject *owner, void *, void *, void *, void *);
    void appendEvent(NativeInputEvent event);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    void clearQueue();
    NativeInputInfo information();
    void readBatch(NativeInputBatch *batch);
    IOReturn newUserClient(task_t task, void *security, UInt32 type,
                          OSDictionary *properties, IOUserClient **handler) override;
};

class NativeBootInputUserClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeBootInputUserClient);
    NativeBootInput *owner;
    bool closed;
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOService *getService() override { return owner; }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch = nullptr, OSObject *target = nullptr, void *reference = nullptr) override;
};
OSDefineMetaClassAndStructors(NativeBootInput, IOService);
OSDefineMetaClassAndStructors(NativeBootInputUserClient, IOUserClient);

bool NativeBootInput::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    queueLock = IOLockAlloc();
    loop = IOWorkLoop::workLoop();
    timer = IOTimerEventSource::timerEventSource(this, poll);
    if (!queueLock || !loop || !timer || loop->addEventSource(timer) != kIOReturnSuccess) return false;
#if defined(__arm64__)
    usb = IONewZero(NativeDWC2, 1);
    if (!usb || !usb->initialize()) {
        IOLog("NativeBootInput: USB controller initialization failed\n");
        return false;
    }
#else
    if (!send_byte(0x64, 0xad) || !send_byte(0x64, 0xa7)) return false;
    for (unsigned i = 0; i < 256 && (port_read(0x64) & 1); ++i) port_read(0x60);
    uint8_t config;
    if (!send_byte(0x64, 0x20) || !receive_byte(false, &config)) return false;
    // Polling owns both ports. Disable their IRQs, enable translation, retain
    // the firmware's other mode bits. Never register an unhandled interrupt.
    config = (config | 0x40) & ~0x33;
    if (!send_byte(0x64, 0x60) || !send_byte(0x60, config) ||
        !send_byte(0x64, 0xae) || !send_byte(0x64, 0xa8) ||
        !device_command(false, 0xf5) || !device_command(false, 0xf0) ||
        !device_command(false, 2) || !device_command(false, 0xf4) ||
        !device_command(true, 0xf6) || !device_command(true, 0xf4)) {
        IOLog("NativeBootInput: controller initialization failed\n");
        return false;
    }
#endif
    running = true;
    if (timer->setTimeoutMS(5) != kIOReturnSuccess) { running = false; return false; }
    setName("NativeBootInput");
    registerService();
    IOLog("NativeBootInput: registered native %s keyboard and mouse\n",
#if defined(__arm64__)
        "DWC2 USB"
#else
        "i8042"
#endif
    );
    return true;
}

void NativeBootInput::stop(IOService *provider)
{
    if (loop) loop->runAction(stopPolling, this);
    if (loop && timer) loop->removeEventSource(timer);
#if defined(__x86_64__)
    send_byte(0x64, 0xad);
    send_byte(0x64, 0xa7);
#else
    if (usb) usb->shutdown();
#endif
    IOService::stop(provider);
}
IOReturn NativeBootInput::stopPolling(OSObject *object, void *, void *, void *, void *)
{
    auto *self = OSDynamicCast(NativeBootInput, object);
    if (!self) return kIOReturnBadArgument;
    self->running = false;
    if (self->timer) self->timer->cancelTimeout();
    return kIOReturnSuccess;
}
void NativeBootInput::free()
{
    if (loop && timer) loop->removeEventSource(timer);
    if (timer) { timer->cancelTimeout(); timer->release(); }
    if (loop) loop->release();
    if (queueLock) IOLockFree(queueLock);
#if defined(__arm64__)
    if (usb) { usb->shutdown(); IODelete(usb, NativeDWC2, 1); }
#endif
    IOService::free();
}
void NativeBootInput::appendEvent(NativeInputEvent event)
{
    IOLockLock(queueLock);
    event.sequence = ++sequence;
    if (count == kNativeInputQueueSize) { ++dropped; }
    else { queue[(head + count) % kNativeInputQueueSize] = event; ++count; }
    IOLockUnlock(queueLock);
}
void NativeBootInput::poll(OSObject *object, IOTimerEventSource *source)
{
    auto *self = OSDynamicCast(NativeBootInput, object);
    if (!self || !self->running) return;
#if defined(__arm64__)
    NativeInputEvent events[24];
    unsigned count = self->usb->poll(events, 24);
    for (unsigned i = 0; i < count; ++i) self->appendEvent(events[i]);
#else
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t status = port_read(0x64);
        if (!(status & 1)) break;
        uint8_t byte = port_read(0x60);
        if (status & 0xc0) { self->decoder = {}; continue; }
        NativeInputEvent event = {};
        int events = (status & 0x20) ? native_ps2_mouse(&self->decoder, byte, &event) :
            native_ps2_keyboard(&self->decoder, byte, &event);
        if (events) self->appendEvent(event);
        if (events == 2) { event.value = 0; self->appendEvent(event); }
    }
#endif
    source->setTimeoutMS(5);
}
void NativeBootInput::clearQueue()
{
    IOLockLock(queueLock);
    head = count = 0;
    IOLockUnlock(queueLock);
}
NativeInputInfo NativeBootInput::information()
{
    IOLockLock(queueLock);
    NativeInputInfo result = {kNativeInputVersion, (1 << kNativeInputKeyboard) | (1 << kNativeInputPointer),
        count, kNativeInputQueueSize, dropped};
    IOLockUnlock(queueLock);
    return result;
}
void NativeBootInput::readBatch(NativeInputBatch *batch)
{
    bzero(batch, sizeof(*batch));
    IOLockLock(queueLock);
    batch->version = kNativeInputVersion;
    batch->dropped = dropped;
    batch->count = count < kNativeInputBatchSize ? count : kNativeInputBatchSize;
    for (unsigned i = 0; i < batch->count; ++i) {
        batch->events[i] = queue[head];
        head = (head + 1) % kNativeInputQueueSize;
    }
    count -= batch->count;
    IOLockUnlock(queueLock);
}
IOReturn NativeBootInput::newUserClient(task_t task, void *security, UInt32 type,
                                       OSDictionary *properties, IOUserClient **handler)
{
    *handler = nullptr;
    if (type != kNativeInputConnection) return kIOReturnBadArgument;
    IOReturn permission = IOUserClient::clientHasPrivilege(security, kIOClientPrivilegeAdministrator);
    if (permission != kIOReturnSuccess) return permission;
    auto *client = OSTypeAlloc(NativeBootInputUserClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) { client->release(); return kIOReturnNoMemory; }
    client->setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);
    client->setProperty(kIOUserClientDefaultLockingSingleThreadExternalMethodKey, kOSBooleanTrue);
    if (!client->attach(this)) { client->release(); return kIOReturnNotAttached; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnExclusiveAccess; }
    *handler = client;
    return kIOReturnSuccess;
}
bool NativeBootInputUserClient::start(IOService *provider)
{
    owner = OSDynamicCast(NativeBootInput, provider);
    closed = false;
    if (!owner || !IOUserClient::start(provider) || !owner->open(this)) return false;
    owner->clearQueue();
    return true;
}
void NativeBootInputUserClient::stop(IOService *provider)
{
    closed = true;
    if (owner && owner->isOpen(this)) owner->close(this);
    IOUserClient::stop(provider);
    owner = nullptr;
}
IOReturn NativeBootInputUserClient::clientClose()
{
    if (closed) return kIOReturnNotOpen;
    closed = true;
    if (owner && owner->isOpen(this)) owner->close(this);
    terminate();
    return kIOReturnSuccess;
}
IOReturn NativeBootInputUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    if (closed || !owner || owner->isInactive()) return kIOReturnNotOpen;
    if (selector != kNativeInputInformation && selector != kNativeInputRead) return kIOReturnUnsupported;
    size_t size = selector == kNativeInputInformation ? sizeof(NativeInputInfo) : sizeof(NativeInputBatch);
    if (!args || args->scalarInputCount || args->structureInputSize || args->structureInputDescriptor ||
        args->scalarOutputCount || args->structureOutputDescriptor || !args->structureOutput ||
        args->structureOutputSize != size) return kIOReturnBadArgument;
    if (selector == kNativeInputInformation) {
        NativeInputInfo info = owner->information();
        memcpy(args->structureOutput, &info, sizeof(info));
    } else owner->readBatch((NativeInputBatch *)args->structureOutput);
    return kIOReturnSuccess;
}
