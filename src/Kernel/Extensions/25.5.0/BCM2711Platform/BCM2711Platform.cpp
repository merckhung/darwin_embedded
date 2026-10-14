// SPDX-License-Identifier: BSD-2-Clause
// Raspberry Pi 4B platform glue for the BCM2711 XNU configuration.
// GIC-400 initialization, acknowledgement and timer/IPI dispatch live in XNU.
#include <IOKit/IOCPU.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOLib.h>
#include <arm/machine_routines.h>
#include <arm/cpu_topology.h>
#include <kern/clock.h>
#include <kern/thread.h>

extern "C" void bcm2711_send_ipi(unsigned int cpu);
extern "C" uint32_t bcm2711_current_interrupt(void);
extern "C" void bcm2711_set_device_interrupt(unsigned int interrupt, bool enabled);
extern "C" unsigned int bcm2711_interrupt_count(void);

class BCM2711CPUInterrupts : public IOCPUInterruptController {
    OSDeclareDefaultStructors(BCM2711CPUInterrupts)
public:
    IOReturn handleInterrupt(void *, IOService *, int source) APPLE_KEXT_OVERRIDE {
        IOInterruptVector *v = &vectors[0];
        if (!v->interruptRegistered) return kIOReturnInvalid;
        v->handler(v->target, v->refCon, v->nub, source);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(BCM2711CPUInterrupts, IOCPUInterruptController)

class BCM2711CPU : public IOCPU {
    OSDeclareDefaultStructors(BCM2711CPU)
    BCM2711CPUInterrupts *interrupts;
public:
    bool registerBootCPU() {
        setCPUNumber(0);
        interrupts = new BCM2711CPUInterrupts;
        if (!interrupts || interrupts->initCPUInterruptController(1, 1) != kIOReturnSuccess)
            return false;
        if (!interrupts->attach(this)) return false;
        interrupts->registerCPUInterruptController();
        ml_processor_info_t info = {};
        info.cpu_id = (cpu_id_t)this;
        info.cluster_type = CLUSTER_TYPE_SMP;
        processor_t processor = NULL;
        ipi_handler_t ipi = NULL;
        perfmon_interrupt_handler_func pmi = NULL;
        if (ml_processor_register(&info, &processor, &ipi, &pmi) != KERN_SUCCESS)
            return false;
        initCPU(true);
        registerService();
        return true;
    }
    void initCPU(bool) APPLE_KEXT_OVERRIDE {
        interrupts->enableCPUInterrupt(this);
        setCPUState(kIOCPUStateRunning);
        bcm2711_send_ipi(0);
    }
    void quiesceCPU() APPLE_KEXT_OVERRIDE {}
    void haltCPU() APPLE_KEXT_OVERRIDE {}
    kern_return_t startCPU(vm_offset_t, vm_offset_t) APPLE_KEXT_OVERRIDE {
        // The boot device tree currently describes CPU0 only. Pi spin-table
        // startup must be implemented before advertising secondary processors.
        return KERN_NOT_SUPPORTED;
    }
    void signalCPU(IOCPU *target) APPLE_KEXT_OVERRIDE {
        if (target == this) bcm2711_send_ipi(0);
    }
    const OSSymbol *getCPUName() APPLE_KEXT_OVERRIDE {
        return OSSymbol::withCStringNoCopy("Cortex-A72");
    }
};
OSDefineMetaClassAndStructors(BCM2711CPU, IOCPU)

class BCM2711DeviceInterrupts : public IOInterruptController {
    OSDeclareDefaultStructors(BCM2711DeviceInterrupts)
    unsigned int lines;
    const OSSymbol *treeName;
    static void connect(void *argument, wait_result_t) {
        BCM2711DeviceInterrupts *self = (BCM2711DeviceInterrupts *)argument;
        IOInterruptController *cpu = OSDynamicCast(IOInterruptController,
            getPlatform()->lookUpInterruptController(gPlatformInterruptControllerName));
        IOInterruptHandler handler = OSMemberFunctionCast(IOInterruptHandler, self,
            &BCM2711DeviceInterrupts::handleInterrupt);
        if (cpu && cpu->registerInterrupt(self, 0, self, handler, NULL) == kIOReturnSuccess) {
            cpu->enableInterrupt(self, 0);
            if (self->treeName) self->publishResource(self->treeName, self);
        }
        self->release();
        thread_terminate(current_thread());
    }
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (!IOInterruptController::start(provider)) return false;
        lines = bcm2711_interrupt_count();
        vectors = (IOInterruptVector *)IOMalloc(lines * sizeof(*vectors));
        if (!vectors) return false;
        bzero(vectors, lines * sizeof(*vectors));
        for (unsigned int i = 0; i < lines; ++i) {
            vectors[i].interruptLock = IOLockAlloc();
            if (!vectors[i].interruptLock) return false;
        }
        getPlatform()->registerInterruptController(
            (OSSymbol *)OSSymbol::withCStringNoCopy("BCM2711GIC"), this);
        IORegistryEntry *gic = IORegistryEntry::fromPath("/arm-io/gic", gIODTPlane);
        if (gic) {
            treeName = IODTInterruptControllerName(gic);
            if (treeName) getPlatform()->registerInterruptController((OSSymbol *)treeName, this);
            gic->release();
        }
        // Registration waits for interrupt delivery, which starts after the
        // platform expert returns. Do that wait on a kernel thread.
        thread_t thread = NULL;
        retain();
        if (kernel_thread_start(connect, this, &thread) != KERN_SUCCESS) {
            release();
            return false;
        }
        thread_deallocate(thread);
        return true;
    }
    IOReturn handleInterrupt(void *, IOService *, int) APPLE_KEXT_OVERRIDE {
        unsigned int id = bcm2711_current_interrupt();
        if (id < 32 || id >= lines) return kIOReturnSuccess;
        IOInterruptVector *v = &vectors[id];
        v->interruptActive = 1;
        if (v->interruptRegistered && !v->interruptDisabledSoft)
            v->handler(v->target, v->refCon, v->nub, v->source);
        else
            v->interruptDisabledSoft = 1;
        if (v->interruptDisabledSoft) {
            v->interruptDisabledHard = 1;
            bcm2711_set_device_interrupt(id, false);
        }
        v->interruptActive = 0;
        return kIOReturnSuccess;
    }
    bool vectorCanBeShared(IOInterruptVectorNumber, IOInterruptVector *) APPLE_KEXT_OVERRIDE {
        return true;
    }
    int getVectorType(IOInterruptVectorNumber, IOInterruptVector *) APPLE_KEXT_OVERRIDE {
        return kIOInterruptTypeLevel;
    }
    void disableVectorHard(IOInterruptVectorNumber id, IOInterruptVector *) APPLE_KEXT_OVERRIDE {
        bcm2711_set_device_interrupt((unsigned int)id, false);
    }
    void enableVector(IOInterruptVectorNumber id, IOInterruptVector *) APPLE_KEXT_OVERRIDE {
        bcm2711_set_device_interrupt((unsigned int)id, true);
    }
};
OSDefineMetaClassAndStructors(BCM2711DeviceInterrupts, IOInterruptController)

class BCM2711Platform : public IODTPlatformExpert {
    OSDeclareDefaultStructors(BCM2711Platform)
    long epoch;
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        const ml_topology_info_t *topology = ml_get_topology_info();
        if (!topology || topology->num_cpus != 1) return false;
        if (!IODTPlatformExpert::start(provider)) return false;
        registerService();
        BCM2711CPU *cpu = new BCM2711CPU;
        if (!cpu || !cpu->init() || !cpu->attach(this) || !cpu->registerBootCPU()) return false;
        BCM2711DeviceInterrupts *gic = new BCM2711DeviceInterrupts;
        if (!gic || !gic->init() || !gic->attach(this) || !gic->start(this)) return false;
        PE_Video video = {};
        if (getConsoleInfo(&video) == kIOReturnSuccess && video.v_baseAddr && video.v_width && video.v_height) {
            IOPlatformDevice *display = OSTypeAlloc(IOPlatformDevice);
            if (!display || !display->init()) return false;
            display->setName("framebuffer");
            if (!display->attach(this)) return false;
            display->registerService();
            IOLog("BCM2711Platform: published framebuffer nub, %ux%u\n",
                (unsigned)video.v_width, (unsigned)video.v_height);
        }
        PE_parse_boot_argn("boot-time", &epoch, sizeof(epoch));
        publishResource("IORTC", this);
        IOLog("BCM2711Platform: Raspberry Pi 4B, Cortex-A72, GIC-400\n");
        return true;
    }
    const char *deleteList() APPLE_KEXT_OVERRIDE { return "('bcm2711-delete-none')"; }
    const char *excludeList() APPLE_KEXT_OVERRIDE { return NULL; }
    bool getMachineName(char *name, int length) APPLE_KEXT_OVERRIDE {
        if (!name || length <= 0) return false;
        strlcpy(name, "arm64", (size_t)length);
        return true;
    }
    bool getModelName(char *name, int length) APPLE_KEXT_OVERRIDE {
        if (!name || length <= 0) return false;
        strlcpy(name, "Raspberry Pi 4B", (size_t)length);
        return true;
    }
    bool getTargetName(char *name, int length) APPLE_KEXT_OVERRIDE { return getModelName(name, length); }
    long getGMTTimeOfDay() APPLE_KEXT_OVERRIDE {
        uint64_t ns;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        return epoch + (long)(ns / NSEC_PER_SEC);
    }
    void setGMTTimeOfDay(long seconds) APPLE_KEXT_OVERRIDE {
        uint64_t ns;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        epoch = seconds - (long)(ns / NSEC_PER_SEC);
    }
};
OSDefineMetaClassAndStructors(BCM2711Platform, IODTPlatformExpert)
