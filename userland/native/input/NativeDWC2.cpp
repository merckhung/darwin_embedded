/* Independent native IOKit register/DMA implementation for BCM2711 DWC2.
 * Register facts checked against QEMU v10.1.0's primary controller model.
 * This is a limited USB2 boot-HID backend, not a general IOUSBHost family. */
#include "NativeDWC2.h"
#include "hid_decode.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>

static uint16_t little16(const uint8_t *bytes) { return bytes[0] | ((uint16_t)bytes[1] << 8); }
static void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }

uint32_t NativeDWC2::read(unsigned offset)
{
    uint32_t value = registers[offset / 4];
    barrier();
    return value;
}
void NativeDWC2::write(unsigned offset, uint32_t value)
{
    barrier();
    registers[offset / 4] = value;
    barrier();
}

int NativeDWC2::transfer(uint8_t address, uint8_t endpoint, uint8_t type, uint8_t speed,
    uint16_t packetSize, bool input, unsigned pid, void *data, unsigned length)
{
    if (!packetSize || packetSize > 64 || length > 256 || (length && !data)) return -1;
    // Channel zero is owned exclusively by the work loop. IN DMA lengths must
    // accommodate whole packets; a short packet establishes the actual length.
    unsigned packets = length ? (length + packetSize - 1) / packetSize : 1;
    unsigned dmaLength = input && length ? packets * packetSize : length;
    if (read(0x500) & (UINT32_C(1) << 31)) return -1;
    bzero(buffer, 256);
    if (!input && length) memcpy(buffer, data, length);
    write(0x508, 0x7ff); /* HCINT: clear prior W1C channel status */
    write(0x50c, 0); /* no IRQ; completion is read from genuine hardware status */
    write(0x504, 0); /* no high-speed transaction translator in this boot topology */
    write(0x510, dmaLength | (packets << 19) | (pid << 29));
    write(0x514, busAddress);
    uint32_t characteristic = packetSize | ((uint32_t)endpoint << 11) | (input ? 1u << 15 : 0) |
        (speed == 2 ? 1u << 17 : 0) | ((uint32_t)type << 18) | (1u << 20) | ((uint32_t)address << 22);
    if (type == 3 && !(read(0x408) & 1)) characteristic |= 1u << 29;
    write(0x500, characteristic | (1u << 31));
    for (unsigned retry = 0; retry < 2000; ++retry) {
        uint32_t status = read(0x508);
        if (status & 2) { /* channel halted */
            if ((status & 1) && !(status & 0x78c)) {
                unsigned remaining = read(0x510) & 0x7ffff;
                if (remaining > dmaLength) return -1;
                unsigned actual = dmaLength - remaining;
                if (actual > length) return -1;
                barrier();
                if (input && actual) memcpy(data, buffer, actual);
                return (int)actual;
            }
            return (status & 0x10) && type == 3 ? -2 : -1; /* NAK is no input */
        }
        IODelay(100);
    }
    write(0x500, characteristic | (1u << 31) | (1u << 30));
    for (unsigned retry = 0; retry < 1000 && (read(0x500) & (1u << 31)); ++retry) IODelay(10);
    IOLog("NativeBootInput: USB transfer timed out addr=%u ep=%u status=%x\n", address, endpoint, read(0x508));
    return -1;
}

int NativeDWC2::control(uint8_t address, uint8_t speed, uint16_t packetSize,
    uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t length)
{
    uint8_t setup[8] = {requestType, request, (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    if (transfer(address, 0, 0, speed, packetSize, false, 3, setup, 8) != 8) return -1;
    bool input = !!(requestType & 0x80);
    int actual = length ? transfer(address, 0, 0, speed, packetSize, input, 2, data, length) : 0;
    if (actual < 0) return actual;
    if (transfer(address, 0, 0, speed, packetSize, !input, 2, nullptr, 0) != 0) return -1;
    return actual;
}

bool NativeDWC2::enumerate(uint8_t address, uint8_t speed, bool root)
{
    uint8_t descriptor[256] = {};
    if (control(0, speed, 8, 0x80, 6, 0x100, 0, descriptor, 8) != 8 || descriptor[1] != 1) return false;
    uint16_t packet = descriptor[7];
    if (packet != 8 && packet != 16 && packet != 32 && packet != 64) return false;
    if (control(0, speed, packet, 0, 5, address, 0, nullptr, 0) != 0) return false;
    IOSleep(10);
    if (control(address, speed, packet, 0x80, 6, 0x100, 0, descriptor, 18) != 18) return false;
    bool hub = descriptor[4] == 9;
    IOLog("NativeBootInput: USB device addr=%u speed=%u class=%u vendor=%x product=%x\n",
        address, speed, descriptor[4], little16(descriptor + 8), little16(descriptor + 10));
    if (control(address, speed, packet, 0x80, 6, 0x200, 0, descriptor, 9) != 9 || descriptor[1] != 2) return false;
    unsigned total = little16(descriptor + 2);
    if (total < 9 || total > sizeof(descriptor) ||
        control(address, speed, packet, 0x80, 6, 0x200, 0, descriptor, total) != (int)total) return false;
    uint8_t configuration = descriptor[5];
    if (!configuration || control(address, speed, packet, 0, 9, configuration, 0, nullptr, 0) != 0) return false;
    if (hub) {
        // Boot topology permits one full-speed hub, with no nested hubs or
        // high-speed split transactions. Reject topologies we cannot drive.
        if (!root || speed != 1 || control(address, speed, packet, 0xa0, 6, 0x2900, 0, descriptor, 9) < 7) return false;
        unsigned ports = descriptor[2], powerDelay = descriptor[5] * 2;
        if (!ports || ports > 8) return false;
        for (unsigned port = 1; port <= ports; ++port)
            if (control(address, speed, packet, 0x23, 3, 8, port, nullptr, 0) != 0) return false;
        IOSleep(powerDelay + 20);
        uint8_t nextAddress = 2;
        for (unsigned port = 1; port <= ports; ++port) {
            uint8_t status[4];
            if (control(address, speed, packet, 0xa3, 0, 0, port, status, 4) != 4) return false;
            if (!(little16(status) & 1)) continue;
            if (control(address, speed, packet, 0x23, 3, 4, port, nullptr, 0) != 0) return false;
            IOSleep(50);
            if (control(address, speed, packet, 0xa3, 0, 0, port, status, 4) != 4 || !(little16(status) & 2)) return false;
            unsigned portStatus = little16(status);
            if (portStatus & (1u << 10)) return false; /* no high-speed hub children */
            const unsigned features[] = {16u, 17u, 20u};
            for (unsigned feature : features)
                if (control(address, speed, packet, 0x23, 1, feature, port, nullptr, 0) != 0) return false;
            if (!enumerate(nextAddress++, portStatus & (1u << 9) ? 2 : 1, false)) return false;
        }
        return true;
    }
    NativeUSBEndpoint candidate = {};
    bool interface = false;
    for (unsigned at = 0; at < total; ) {
        unsigned length = descriptor[at];
        if (length < 2 || length > total - at) return false;
        const uint8_t *item = descriptor + at;
        if (item[1] == 4) {
            if (length < 9) return false;
            interface = item[3] == 0 && item[5] == 3 && item[6] == 1 && (item[7] == 1 || item[7] == 2);
            if (interface) { candidate.interfaceNumber = item[2]; candidate.protocol = item[7]; }
        } else if (item[1] == 5 && interface) {
            if (length < 7 || !(item[2] & 0x80) || (item[3] & 3) != 3) return false;
            candidate.address = address;
            candidate.number = item[2] & 15;
            candidate.speed = speed;
            candidate.packetSize = little16(item + 4);
            break;
        }
        at += length;
    }
    if (!candidate.number || candidate.packetSize > 64 ||
        candidate.packetSize < (candidate.protocol == 1 ? 8 : 3) || endpointCount == 2 ||
        (capabilities & (1u << candidate.protocol))) return false;
    if (control(address, speed, packet, 0x21, 0x0b, 0, candidate.interfaceNumber, nullptr, 0) != 0 ||
        control(address, speed, packet, 0x21, 0x0a, 0, candidate.interfaceNumber, nullptr, 0) != 0) return false;
    endpoints[endpointCount++] = candidate;
    capabilities |= 1u << candidate.protocol;
    IOLog("NativeBootInput: USB boot HID addr=%u ep=%u protocol=%u packet=%u\n",
        address, candidate.number, candidate.protocol, candidate.packetSize);
    return true;
}

bool NativeDWC2::initialize()
{
    // This MMIO address belongs to BCM2711; never touch it on another board.
    IORegistryEntry *root = IORegistryEntry::fromPath("/", gIODTPlane);
    bool board = false;
    OSData *compatible = root ? OSDynamicCast(OSData, root->getProperty("compatible")) : nullptr;
    if (compatible) {
        const char *bytes = (const char *)compatible->getBytesNoCopy();
        unsigned size = compatible->getLength();
        for (unsigned at = 0; bytes && at < size;) {
            size_t length = strnlen(bytes + at, size - at);
            if (length == size - at) break;
            if ((length == 7 && !memcmp(bytes + at, "bcm2711", 7)) ||
                (length == 12 && !memcmp(bytes + at, "brcm,bcm2711", 12)) ||
                (length == 21 && !memcmp(bytes + at, "raspberrypi,4-model-b", 21))) board = true;
            at += length + 1;
        }
    }
    if (root) root->release();
    if (!board) return false;
    memory = IODeviceMemory::withRange(0xfe980000, 0x1000);
    mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (!mapping) return false;
    registers = (volatile uint32_t *)mapping->getVirtualAddress();
    uint32_t identity = read(0x40);
    if ((identity >> 16) != 0x4f54) return false;
    dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOMapInhibitCache, 4096, 0x3ffff000ULL);
    if (!dma || dma->prepare(kIODirectionInOut) != kIOReturnSuccess) return false;
    dmaPrepared = true;
    IOByteCount contiguous = 0;
    IOPhysicalAddress physical = dma->getPhysicalSegment(0, &contiguous);
    if (!physical || physical >= 0x40000000 || contiguous < 4096) return false;
    buffer = (uint8_t *)dma->getBytesNoCopy();
    busAddress = (uint32_t)physical | 0xc0000000u;
    controllerOwned = true;
    write(0x18, 0);
    write(0x08, 1u << 5); /* native DMA, global IRQ disabled */
    write(0x0c, (read(0x0c) & ~(1u << 30)) | (1u << 29));
    write(0xe00, 0); /* ungate host clocks */
    IOSleep(25);
    if (!(read(0x440) & 1)) return false;
    write(0x400, 1); /* 48-MHz full/low-speed clock */
    write(0x440, (1u << 12) | (1u << 8));
    IOSleep(60);
    write(0x440, 1u << 12);
    IOSleep(20);
    uint32_t port = read(0x440);
    if (!(port & 4)) return false;
    uint8_t speed = (port >> 17) & 3;
    IOLog("NativeBootInput: DWC2 id=%x port=%x DMA=%x\n", identity, port, busAddress);
    if (!enumerate(1, speed, true) || capabilities != 6) return false;
    return true;
}

void NativeDWC2::shutdown()
{
    if (registers && controllerOwned) {
        uint32_t channel = read(0x500);
        if (channel & (1u << 31)) write(0x500, channel | (1u << 30));
        write(0x08, 0); /* stop new DMA requests before releasing any buffer */
        unsigned retry = 0;
        while (((read(0x500) & (1u << 31)) || !(read(0x10) & (1u << 31))) && retry++ < 10000) IODelay(10);
        if (retry > 10000) {
            IOLog("NativeBootInput: DMA did not quiesce; retaining live USB mappings\n");
            return; /* preserve mapped memory rather than allow live DMA into freed pages */
        }
        write(0x440, read(0x440) & ~((1u << 12) | 0x2e));
        controllerOwned = false;
    }
    if (dma) { if (dmaPrepared) dma->complete(kIODirectionInOut); dma->release(); dma = nullptr; dmaPrepared = false; }
    if (mapping) { mapping->release(); mapping = nullptr; }
    if (memory) { memory->release(); memory = nullptr; }
    registers = nullptr;
}

unsigned NativeDWC2::poll(NativeInputEvent *events, unsigned capacity)
{
    unsigned count = 0;
    if (!registers || capacity < 24 || !(read(0x440) & 1)) return 0;
    for (unsigned device = 0; device < endpointCount; ++device) {
        NativeUSBEndpoint &endpoint = endpoints[device];
        uint8_t report[64] = {};
        int actual = transfer(endpoint.address, endpoint.number, 3, endpoint.speed,
            endpoint.packetSize, true, endpoint.pid, report, endpoint.packetSize);
        if (actual == -2) continue;
        if (actual < 0) { IOLog("NativeBootInput: USB input transfer failed addr=%u\n", endpoint.address); continue; }
        endpoint.pid ^= 2;
        if (endpoint.protocol == 2) {
            count += native_hid_mouse(endpoint.previous, report, actual, events + count);
        } else {
            count += native_hid_keyboard(endpoint.previous, report, actual, events + count, capacity - count);
        }
    }
    return count;
}
