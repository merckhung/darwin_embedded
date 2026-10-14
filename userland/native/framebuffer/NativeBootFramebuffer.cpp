/*
 * Native Darwin boot-framebuffer access for the source-built desktop port.
 * Exposes the actual validated PE_Video aperture through IOUserClient mapping.
 * This implements a small explicit interface, not an IOFramebuffer substitute.
 */
#include "NativeBootFramebuffer.h"
#include "geometry.h"
#include "bootloader/video/metadata.h"
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <mach/vm_param.h>

class NativeBootFramebuffer : public IOService {
    OSDeclareDefaultStructors(NativeBootFramebuffer);
    IOPhysicalAddress physicalBase;
    IOMemoryDescriptor *memory;
    NativeFramebufferInfo information;
    NativeFramebufferPixelLayout pixelLayout;
public:
    bool start(IOService *provider) override;
    void free() override;
    IOReturn newUserClient(task_t task, void *security, UInt32 type,
                          OSDictionary *properties, IOUserClient **handler) override;
    const NativeFramebufferInfo &info() const { return information; }
    const NativeFramebufferPixelLayout &pixels() const { return pixelLayout; }
    IOMemoryDescriptor *copyMemory() const;
};

class NativeBootFramebufferUserClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeBootFramebufferUserClient);
    NativeBootFramebuffer *owner;
    bool closed;
    bool consoleClaimed;
    IOReturn setConsoleOwnership(bool claim);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOService *getService() override { return owner; }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch = nullptr,
                            OSObject *target = nullptr, void *reference = nullptr) override;
};

OSDefineMetaClassAndStructors(NativeBootFramebuffer, IOService);
OSDefineMetaClassAndStructors(NativeBootFramebufferUserClient, IOUserClient);

bool NativeBootFramebuffer::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    PE_Video video = {};
    IOPlatformExpert *platform = getPlatform();
    if (!platform || platform->getConsoleInfo(&video) != kIOReturnSuccess) return false;
    uint64_t base = video.v_baseAddr & ~UINT64_C(3);
    // Only whole, reserved framebuffer pages are exported. Reject formats and
    // partial pages whose safely mappable extent the boot ABI cannot establish.
    uint64_t length = native_framebuffer_length(base, video.v_width, video.v_height,
        video.v_rowBytes, video.v_depth, video.v_offset, video.v_length, video.v_rotate, PAGE_MAX_SIZE);
    if (!length) {
        IOLog("NativeBootFramebuffer: unsupported boot scanout geometry\n");
        return false;
    }
    pixelLayout = {};
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSObject *property = chosen ? chosen->copyProperty(NATIVE_BOOT_VIDEO_PROPERTY) : nullptr;
    if (chosen) chosen->release();
    if (property) {
        const OSData *data = OSDynamicCast(OSData, property);
        struct native_boot_video metadata = {};
        bool valid = data && data->getLength() == sizeof(metadata);
        if (valid) {
            memcpy(&metadata, data->getBytesNoCopy(), sizeof(metadata));
            valid = native_boot_video_matches(&metadata, base, length,
                (uint32_t)video.v_width, (uint32_t)video.v_height, (uint32_t)video.v_rowBytes);
        }
        property->release();
        if (!valid) {
            IOLog("NativeBootFramebuffer: rejected inconsistent firmware pixel metadata\n");
            return false;
        }
        pixelLayout = {1,
            metadata.format == NATIVE_BOOT_VIDEO_RGBX ? UINT32_C(0x000000ff) : UINT32_C(0x00ff0000),
            UINT32_C(0x0000ff00),
            metadata.format == NATIVE_BOOT_VIDEO_RGBX ? UINT32_C(0x00ff0000) : UINT32_C(0x000000ff),
            UINT32_C(0xff000000)};
    }
    physicalBase = (IOPhysicalAddress)base;
    memory = IODeviceMemory::withRange(physicalBase, length);
    if (!memory) return false;
    information = {kNativeFramebufferABIVersion, (uint32_t)video.v_width, (uint32_t)video.v_height,
                   (uint32_t)video.v_rowBytes, (uint32_t)video.v_depth, (uint32_t)PAGE_MAX_SIZE, length};
    setName("NativeBootFramebuffer");
    registerService();
    IOLog("NativeBootFramebuffer: registered %ux%u stride %u, %llu bytes\n",
          information.width, information.height, information.row_bytes, information.byte_length);
    return true;
}

IOMemoryDescriptor *NativeBootFramebuffer::copyMemory() const
{
    // XNU's unmap operation looks up the existing mapping on this descriptor.
    // Preserve identity across all calls, rather than manufacturing new ranges.
    memory->retain();
    return memory;
}

void NativeBootFramebuffer::free()
{
    if (memory) memory->release();
    IOService::free();
}

IOReturn NativeBootFramebuffer::newUserClient(task_t task, void *security, UInt32 type,
                                             OSDictionary *properties, IOUserClient **handler)
{
    *handler = nullptr;
    if (type != kNativeFramebufferConnection) return kIOReturnBadArgument;
    IOReturn permission = IOUserClient::clientHasPrivilege(security, kIOClientPrivilegeAdministrator);
    if (permission != kIOReturnSuccess) return permission;
    auto *client = OSTypeAlloc(NativeBootFramebufferUserClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) {
        client->release();
        return kIOReturnNoMemory;
    }
    client->setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);
    client->setProperty(kIOUserClientDefaultLockingSingleThreadExternalMethodKey, kOSBooleanTrue);
    if (!client->attach(this)) {
        client->release();
        return kIOReturnNotAttached;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnExclusiveAccess;
    }
    *handler = client;
    return kIOReturnSuccess;
}

bool NativeBootFramebufferUserClient::start(IOService *provider)
{
    owner = OSDynamicCast(NativeBootFramebuffer, provider);
    closed = false;
    consoleClaimed = false;
    return owner && IOUserClient::start(provider) && owner->open(this);
}

IOReturn NativeBootFramebufferUserClient::setConsoleOwnership(bool claim)
{
    if (claim == consoleClaimed)
        return claim ? kIOReturnExclusiveAccess : kIOReturnNotOpen;
    IOPlatformExpert *platform = getPlatform();
    if (!platform) return kIOReturnNotReady;
    IOReturn result = platform->setConsoleInfo(nullptr, claim ? kPEDisableScreen : kPEEnableScreen);
    if (result == kIOReturnSuccess) {
        consoleClaimed = claim;
        IOLog("NativeBootFramebuffer: console %s\n", claim ? "claimed" : "restored");
    }
    return result;
}

void NativeBootFramebufferUserClient::stop(IOService *provider)
{
    if (consoleClaimed && setConsoleOwnership(false) != kIOReturnSuccess)
        IOLog("NativeBootFramebuffer: console restore failed during stop\n");
    closed = true;
    if (owner && owner->isOpen(this)) owner->close(this);
    IOUserClient::stop(provider);
    owner = nullptr;
}

IOReturn NativeBootFramebufferUserClient::clientClose()
{
    if (closed) return kIOReturnNotOpen;
    IOReturn result = consoleClaimed ? setConsoleOwnership(false) : kIOReturnSuccess;
    closed = true;
    if (owner && owner->isOpen(this)) owner->close(this);
    terminate();
    return result;
}

IOReturn NativeBootFramebufferUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                             IOMemoryDescriptor **memory)
{
    *memory = nullptr;
    if (closed || !owner || owner->isInactive()) return kIOReturnNotOpen;
    if (type != kNativeFramebufferMemory) return kIOReturnBadArgument;
    *options = 0;
    *memory = owner->copyMemory();
    return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn NativeBootFramebufferUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *arguments, IOExternalMethodDispatch *, OSObject *, void *)
{
    if (closed || !owner || owner->isInactive()) return kIOReturnNotOpen;
    if (selector == kNativeFramebufferAcquireConsole || selector == kNativeFramebufferReleaseConsole) {
        if (!arguments || arguments->scalarInputCount || arguments->structureInputSize ||
            arguments->structureInputDescriptor || arguments->scalarOutputCount ||
            arguments->structureOutputDescriptor || arguments->structureOutputSize)
            return kIOReturnBadArgument;
        return setConsoleOwnership(selector == kNativeFramebufferAcquireConsole);
    }
    const void *output;
    size_t outputSize;
    if (selector == kNativeFramebufferInformation) {
        output = &owner->info();
        outputSize = sizeof(NativeFramebufferInfo);
    } else if (selector == kNativeFramebufferPixelLayout && owner->pixels().version) {
        output = &owner->pixels();
        outputSize = sizeof(NativeFramebufferPixelLayout);
    } else return kIOReturnUnsupported;
    if (!arguments || arguments->scalarInputCount || arguments->structureInputSize ||
        arguments->structureInputDescriptor ||
        arguments->scalarOutputCount || arguments->structureOutputDescriptor ||
        !arguments->structureOutput || arguments->structureOutputSize != outputSize)
        return kIOReturnBadArgument;
    memcpy(arguments->structureOutput, output, outputSize);
    return kIOReturnSuccess;
}
