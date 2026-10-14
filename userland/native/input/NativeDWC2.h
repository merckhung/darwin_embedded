/* Native BCM2711 USB2 host backend; bounded boot HID devices, no USB family emulation. */
#ifndef NATIVE_DWC2_H
#define NATIVE_DWC2_H
#include "NativeBootInput.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>

struct NativeUSBEndpoint {
    uint8_t address, number, interfaceNumber, protocol, speed, pid;
    uint16_t packetSize;
    uint8_t previous[8];
};
struct NativeDWC2 {
    IODeviceMemory *memory;
    IOMemoryMap *mapping;
    IOBufferMemoryDescriptor *dma;
    volatile uint32_t *registers;
    uint8_t *buffer;
    uint32_t busAddress;
    NativeUSBEndpoint endpoints[2];
    unsigned endpointCount;
    uint32_t capabilities;
    bool controllerOwned, dmaPrepared;
    bool initialize();
    void shutdown();
    unsigned poll(NativeInputEvent *events, unsigned capacity);
private:
    uint32_t read(unsigned offset);
    void write(unsigned offset, uint32_t value);
    int transfer(uint8_t address, uint8_t endpoint, uint8_t type, uint8_t speed,
        uint16_t packetSize, bool input, unsigned pid, void *data, unsigned length);
    int control(uint8_t address, uint8_t speed, uint16_t packetSize,
        uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t length);
    bool enumerate(uint8_t address, uint8_t speed, bool root);
};
#endif
