// SPDX-License-Identifier: BSD-2-Clause
// PC UEFI entry for the embedded XNU collection and HFS+ RAM-disk profile.
#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stddef.h>
#include <cpuid.h>
#include "src/Kernel/xnu/12377.121.6/pexpert/pexpert/i386/boot.h"
#include "bootloader/video/metadata.h"
#include "image.h"
#include "layout.h"

#define FW(function, count, ...) uefi_call_wrapper(function, count, __VA_ARGS__)
#define BOOT_BASE ((uint64_t)PC_BOOT_BASE)
#define TABLE_BASE (BOOT_BASE + 0x1000)
#define TREE_BASE (BOOT_BASE + 0x6000)
#define TREE_CAPACITY 0x8000
#define MAP_BASE (BOOT_BASE + 0x10000)
#define MAP_CAPACITY 0x10000
#define RUNTIME_BASE ((uint64_t)PC_RUNTIME_BASE)
#define KERNEL_BASE ((uint64_t)PC_KERNEL_BASE)
#define KERNEL_ALIAS 0xffffff8000000000ULL

_Static_assert(sizeof(boot_args) == 4096, "XNU boot argument ABI");
_Static_assert(sizeof(EFI_SYSTEM_TABLE) <= 0x200, "EFI table slot");
_Static_assert(sizeof(EFI_RUNTIME_SERVICES) <= 0x200, "EFI runtime slot");
_Static_assert(sizeof(EfiMemoryRange) == sizeof(EFI_MEMORY_DESCRIPTOR), "EFI memory ABI");

// The bridge changes only the stack. XNU receives a normal SysV 64-bit call
// and replaces firmware page tables itself; no 32-bit loader stub is needed.
extern void pc_enter_on_private_stack(uint64_t staging, uint64_t size, uint64_t entry,
                                     uint64_t arguments, uint64_t stack) __attribute__((noreturn));
static EFI_BOOT_SERVICES *services;
static EFI_SYSTEM_TABLE *firmware;
static EFI_FILE_PROTOCOL *volume;
static EFI_LOADED_IMAGE *loaded;
static uint64_t staging, stack_top, ramdisk_base, ramdisk_size, bus_frequency, tsc_frequency;
static struct pc_image collection;
static struct native_boot_video scanout;
static uint8_t entropy[256];
static boot_args *arguments = (boot_args *)(uintptr_t)BOOT_BASE;

static void copy(void *destination, const void *source, size_t size) {
    uint8_t *to = destination;
    const uint8_t *from = source;
    for (size_t i = 0; i < size; ++i) to[i] = from[i];
}
// Runs after ExitBootServices on the private stack selected by enter64.S.
// The EFI image, staging area and this stack were checked against the kernel
// destination. Copying cannot overwrite the instructions or stack in use.
// Keep the firmware's identity mapping active until execution reaches XNU.
__attribute__((noreturn, noinline))
void pc_install_collection(uint64_t source, uint64_t bytes, uint64_t entry,
                           uint64_t boot_arguments) {
    copy((void *)(uintptr_t)KERNEL_BASE, (const void *)(uintptr_t)source, (size_t)bytes);
    typedef void (*kernel_entry)(boot_args *);
    ((kernel_entry)(uintptr_t)entry)((boot_args *)(uintptr_t)boot_arguments);
    __builtin_trap(); // A kernel entry has no return path into firmware.
}

static void clear(void *destination, size_t size) {
    uint8_t *to = destination;
    for (size_t i = 0; i < size; ++i) to[i] = 0;
}
static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static int equal(const void *left, const void *right, size_t size) {
    const uint8_t *a = left, *b = right;
    for (size_t i = 0; i < size; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static uint32_t read32(const void *data) { uint32_t value; copy(&value, data, 4); return value; }
static uint64_t read64(const void *data) { uint64_t value; copy(&value, data, 8); return value; }
static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t value; __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port)); return value;
}
static void serial(const char *text) {
    while (*text) {
        // Bound the poll so PCs without a COM1 port can still boot.
        for (unsigned int n = 0; n < 100000 && !(in8(0x3fd) & 0x20); ++n) {}
        out8(0x3f8, (uint8_t)*text++);
    }
}
static void hex(uint64_t value) {
    char text[19] = "0x0000000000000000";
    for (unsigned int i = 0; i < 16; ++i) text[17 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
    serial(text);
}
static EFI_STATUS failure(const char *phase, EFI_STATUS status) {
    serial("PC UEFI: "); serial(phase); serial(" failed "); hex(status); serial("\r\n");
    Print(L"PC UEFI: %a failed: %r\r\n", phase, status);
    return status;
}
static void stop(const char *phase, EFI_STATUS status) __attribute__((noreturn));
static void stop(const char *phase, EFI_STATUS status) {
    serial("PC UEFI: "); serial(phase); serial(" failed "); hex(status); serial("\r\n");
    for (;;) __asm__ volatile("cli; hlt");
}

static EFI_STATUS pages(uint64_t bytes, EFI_MEMORY_TYPE type, uint64_t *address) {
    EFI_PHYSICAL_ADDRESS at = 0xffffffffULL;
    if (!bytes || bytes > 0xfffff000ULL) return EFI_BAD_BUFFER_SIZE;
    EFI_STATUS status = FW(services->AllocatePages, 4, AllocateMaxAddress, type,
                           (UINTN)((bytes + 4095) / 4096), &at);
    if (!EFI_ERROR(status)) { *address = at; clear((void *)(uintptr_t)at, (bytes + 4095) & ~4095ULL); }
    return status;
}
static EFI_STATUS read_file(CHAR16 *name, uint64_t limit, void **data, UINTN *size) {
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status = FW(volume->Open, 5, volume, &file, name, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;
    UINTN info_size = 0;
    status = FW(file->GetInfo, 4, file, &info_guid, &info_size, NULL);
    EFI_FILE_INFO *info = NULL;
    if (status != EFI_BUFFER_TOO_SMALL || info_size < SIZE_OF_EFI_FILE_INFO || info_size > 65536) {
        FW(file->Close, 1, file); return EFI_COMPROMISED_DATA;
    }
    status = FW(services->AllocatePool, 3, EfiLoaderData, info_size, (void **)&info);
    if (EFI_ERROR(status)) { FW(file->Close, 1, file); return status; }
    status = FW(file->GetInfo, 4, file, &info_guid, &info_size, info);
    if (!EFI_ERROR(status) && (info->FileSize == 0 || info->FileSize > limit ||
                              (info->Attribute & EFI_FILE_DIRECTORY))) status = EFI_BAD_BUFFER_SIZE;
    if (!EFI_ERROR(status)) {
        *size = (UINTN)info->FileSize;
        status = FW(services->AllocatePool, 3, EfiLoaderData, *size, data);
        if (!EFI_ERROR(status)) {
            UINTN actual = *size;
            status = FW(file->Read, 3, file, &actual, *data);
            if (!EFI_ERROR(status) && actual != *size) status = EFI_DEVICE_ERROR;
            if (EFI_ERROR(status)) { FW(services->FreePool, 1, *data); *data = NULL; }
        }
    }
    FW(services->FreePool, 1, info);
    FW(file->Close, 1, file);
    return status;
}

struct rng_protocol {
    void *get_info;
    EFI_STATUS (EFIAPI *get_rng)(struct rng_protocol *, EFI_GUID *, UINTN, uint8_t *);
};
static EFI_STATUS seed_boot(void) {
    EFI_GUID rng_guid = {0x3152bca5,0xeade,0x433d,{0x86,0x2e,0xc0,0x1c,0xdc,0x29,0x1f,0x44}};
    struct rng_protocol *rng = NULL;
    EFI_STATUS status = FW(services->LocateProtocol, 3, &rng_guid, NULL, (void **)&rng);
    if (!EFI_ERROR(status)) status = FW(rng->get_rng, 4, rng, NULL, sizeof(entropy), entropy);
    if (EFI_ERROR(status)) {
        void *data = NULL; UINTN size = 0;
        status = read_file(L"\\EFI\\BOOT\\entropy.bin", sizeof(entropy), &data, &size);
        if (EFI_ERROR(status)) return status;
        if (size != sizeof(entropy)) { FW(services->FreePool, 1, data); return EFI_BAD_BUFFER_SIZE; }
        copy(entropy, data, size); clear(data, size); FW(services->FreePool, 1, data);
    }
    uint8_t present = 0;
    for (unsigned int i = 0; i < sizeof(entropy); ++i) present |= entropy[i];
    return present ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

static EFI_STATUS boot_command_line(void) {
    void *data = NULL; UINTN size = 0;
    EFI_STATUS status = read_file(L"\\EFI\\BOOT\\boot-args.txt", 900, &data, &size);
    if (status == EFI_NOT_FOUND) {
        const char *defaults = "-v -s serial=3 debug=0x14c keepsyms=1";
        copy(arguments->CommandLine, defaults, length(defaults) + 1);
    } else if (!EFI_ERROR(status)) {
        const uint8_t *bytes = data;
        while (size && (bytes[size - 1] == '\r' || bytes[size - 1] == '\n' || bytes[size - 1] == ' ')) --size;
        for (UINTN i = 0; i < size; ++i) {
            if (bytes[i] < 32 || bytes[i] > 126) { FW(services->FreePool, 1, data); return EFI_INVALID_PARAMETER; }
        }
        copy(arguments->CommandLine, data, size);
        FW(services->FreePool, 1, data);
    } else return status;
    // This loader's root contract is always the RAM disk loaded below.
    size_t used = length(arguments->CommandLine);
    const char *suffix = " rd=md0 rootfstype=hfs";
    for (size_t i = 0; i < used; ++i)
        if ((i == 0 || arguments->CommandLine[i - 1] == ' ') &&
            equal(arguments->CommandLine + i, "rd=", 3)) return EFI_INVALID_PARAMETER;
    copy(arguments->CommandLine + used, suffix, length(suffix) + 1);
    return EFI_SUCCESS;
}

static EFI_STATUS video(void) {
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = FW(services->LocateProtocol, 3, &guid, NULL, (void **)&gop);
    if (status == EFI_NOT_FOUND) return EFI_SUCCESS; // Serial-only PC.
    if (EFI_ERROR(status)) return status;
    // Prefer the same modest scanout size on OVMF and physical GOP firmware.
    for (UINT32 i = 0; i < gop->Mode->MaxMode; ++i) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL; UINTN bytes = 0;
        status = FW(gop->QueryMode, 4, gop, i, &bytes, &info);
        int desired = !EFI_ERROR(status) && bytes >= sizeof(*info) &&
            info->HorizontalResolution == 1024 && info->VerticalResolution == 768 &&
            info->PixelFormat <= PixelBlueGreenRedReserved8BitPerColor;
        if (info) FW(services->FreePool, 1, info);
        if (desired) { status = FW(gop->SetMode, 2, gop, i); if (EFI_ERROR(status)) return status; break; }
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    uint64_t stride = (uint64_t)info->PixelsPerScanLine * 4;
    uint64_t base = gop->Mode->FrameBufferBase, size = gop->Mode->FrameBufferSize;
    if (!base || !info->HorizontalResolution || !info->VerticalResolution ||
        stride < (uint64_t)info->HorizontalResolution * 4 || stride > UINT32_MAX ||
        info->VerticalResolution > size / stride || base > UINT64_MAX - size ||
        info->PixelFormat > PixelBlueGreenRedReserved8BitPerColor) return EFI_UNSUPPORTED;
    scanout = (struct native_boot_video){NATIVE_BOOT_VIDEO_VERSION,
        info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ? NATIVE_BOOT_VIDEO_RGBX : NATIVE_BOOT_VIDEO_BGRX,
        base, size, info->HorizontalResolution, info->VerticalResolution, (uint32_t)stride, 32};
    arguments->Video = (Boot_Video){.v_display = FB_TEXT_MODE, .v_rowBytes = scanout.row_bytes,
        .v_width = scanout.width, .v_height = scanout.height, .v_depth = 32, .v_baseAddr = base | 1};
    if (base <= UINT32_MAX)
        arguments->VideoV1 = (Boot_VideoV1){(uint32_t)base, FB_TEXT_MODE, scanout.row_bytes,
            scanout.width, scanout.height, 32};
    return EFI_SUCCESS;
}

static uint64_t counter(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return lo | (uint64_t)hi << 32;
}
static void clock_frequency(void) {
    uint64_t before = counter();
    FW(services->Stall, 1, 20000);
    tsc_frequency = (counter() - before) * 50;
    bus_frequency = 100000000;
    unsigned int a, b, c, d;
    __cpuid(1, a, b, c, d);
    unsigned int family = (a >> 8) & 15, model = ((a >> 4) & 15) | ((a >> 12) & 0xf0);
    __cpuid(0, a, b, c, d);
    if (b == 0x756e6547 && c == 0x6c65746e && d == 0x49656e69 && family == 6 && model == 23) {
        // XNU derives Penryn's TSC rate from IA32_PERF_STATUS and FSB rate.
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x198));
        unsigned int twice_ratio = ((hi >> 8) & 31) * 2 + ((hi >> 14) & 1);
        if (twice_ratio) bus_frequency = tsc_frequency * 2 / twice_ratio;
    }
    arguments->FSBFrequency = bus_frequency;
}

// XNU's device-tree wire format: native LE property lengths, four-byte padding.
// The bounded writer never allocates firmware pool objects for individual nodes.
struct tree_writer { uint8_t *base; size_t used; int failed; };
static void emit(struct tree_writer *w, const void *data, size_t bytes) {
    if (bytes > TREE_CAPACITY - w->used) { w->failed = 1; return; }
    copy(w->base + w->used, data, bytes); w->used += bytes;
}
static void property(struct tree_writer *w, const char *name, const void *data, uint32_t bytes) {
    struct { char name[32]; uint32_t size; } header = {{0}, bytes};
    if (length(name) >= sizeof(header.name)) { w->failed = 1; return; }
    copy(header.name, name, length(name)); emit(w, &header, sizeof(header)); emit(w, data, bytes);
    uint32_t zero = 0; emit(w, &zero, (4 - bytes % 4) % 4);
}
static void text_property(struct tree_writer *w, const char *name, const char *text) {
    property(w, name, text, (uint32_t)length(text) + 1);
}
static void node(struct tree_writer *w, const char *name, uint32_t properties, uint32_t children) {
    uint32_t header[2] = {properties + 1, children}; emit(w, header, sizeof(header));
    text_property(w, "name", name);
}
static void guid_text(const EFI_GUID *guid, char name[37]) {
    const uint8_t *bytes = (const uint8_t *)guid;
    const uint8_t order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    unsigned int at = 0;
    for (unsigned int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) name[at++] = '-';
        name[at++] = "0123456789abcdef"[bytes[order[i]] >> 4];
        name[at++] = "0123456789abcdef"[bytes[order[i]] & 15];
    }
    name[36] = 0;
}

static int hardware_uuid(uint8_t uuid[16]) {
    EFI_GUID smbios2 = {0xeb9d2d31,0x2d88,0x11d3,{0x9a,0x16,0,0x90,0x27,0x3f,0xc1,0x4d}};
    EFI_GUID smbios3 = {0xf2fd1544,0x9794,0x4a2c,{0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94}};
    for (UINTN i = 0; i < firmware->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *table = &firmware->ConfigurationTable[i];
        const uint8_t *entry = table->VendorTable, *data = NULL;
        size_t bytes = 0;
        if (!entry) continue;
        if (equal(&table->VendorGuid, &smbios3, 16) && equal(entry, "_SM3_", 5) && entry[6] >= 24) {
            data = (const void *)(uintptr_t)read64(entry + 16); bytes = read32(entry + 12);
        } else if (equal(&table->VendorGuid, &smbios2, 16) && equal(entry, "_SM_", 4) && entry[5] >= 31) {
            data = (const void *)(uintptr_t)read32(entry + 24); bytes = entry[22] | (size_t)entry[23] << 8;
        }
        if (!data || bytes > 16 * 1024 * 1024) continue;
        size_t at = 0;
        while (bytes - at >= 4) {
            unsigned int type = data[at], size = data[at + 1];
            if (size < 4 || size > bytes - at || type == 127) break;
            if (type == 1 && size >= 24) {
                uint8_t any = 0, all = 0xff;
                for (unsigned int n = 0; n < 16; ++n) { any |= data[at + 8 + n]; all &= data[at + 8 + n]; }
                if (any && all != 0xff) {
                    const uint8_t order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
                    for (unsigned int n = 0; n < 16; ++n) uuid[n] = data[at + 8 + order[n]];
                    return 1;
                }
            }
            at += size;
            while (bytes - at >= 2 && (data[at] || data[at + 1])) ++at;
            if (bytes - at < 2) break;
            at += 2;
        }
    }
    return 0;
}

static EFI_STATUS device_tree(void) {
    EFI_GUID acpi2 = ACPI_20_TABLE_GUID, acpi1 = ACPI_TABLE_GUID;
    struct tree_writer w = {(void *)(uintptr_t)TREE_BASE, 0, 0};
    node(&w, "device-tree", 2, 4);
    text_property(&w, "compatible", "ACPI"); text_property(&w, "model", "Darwin Embedded PC");
    node(&w, "chosen", 4, 1);
    text_property(&w, "boot-args", arguments->CommandLine);
    text_property(&w, "boot-file", "\\EFI\\BOOT\\kernel");
    property(&w, "random-seed", entropy, sizeof(entropy));
    property(&w, NATIVE_BOOT_VIDEO_PROPERTY, &scanout, sizeof(scanout));
    node(&w, "memory-map", 1, 0);
    uint64_t disk[2] = {ramdisk_base, ramdisk_size}; property(&w, "RAMDisk", disk, sizeof(disk));
    node(&w, "options", 1, 0);
    // Prefer the PC's hardware identity. Without SMBIOS, publish an explicitly
    // per-boot fallback so IOBSDGetPlatformUUID can still complete.
    uint8_t uuid[16]; copy(uuid, entropy, sizeof(uuid)); uuid[6] = (uuid[6] & 15) | 0x40; uuid[8] = (uuid[8] & 63) | 0x80;
    hardware_uuid(uuid);
    property(&w, "platform-uuid", uuid, sizeof(uuid));
    node(&w, "defaults", 0, 0);
    node(&w, "efi", 2, 4);
    text_property(&w, "firmware-abi", "EFI64");
    property(&w, "firmware-revision", &firmware->FirmwareRevision, 4);
    node(&w, "platform", 3, 0);
    property(&w, "FSBFrequency", &bus_frequency, 8); property(&w, "TSCFrequency", &tsc_frequency, 8);
    property(&w, "system-id", uuid, sizeof(uuid));
    node(&w, "configuration-table", 0, (uint32_t)firmware->NumberOfTableEntries);
    for (UINTN i = 0; i < firmware->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *table = &firmware->ConfigurationTable[i];
        char name[37]; guid_text(&table->VendorGuid, name);
        const char *alias = equal(&table->VendorGuid, &acpi2, 16) ? "ACPI_20" :
                            equal(&table->VendorGuid, &acpi1, 16) ? "ACPI" : NULL;
        node(&w, name, alias ? 3 : 2, 0);
        property(&w, "guid", &table->VendorGuid, 16);
        uint64_t address = (uintptr_t)table->VendorTable; property(&w, "table", &address, 8);
        if (alias) text_property(&w, "alias", alias);
    }
    node(&w, "runtime-services", 1, 0);
    uint64_t runtime = TABLE_BASE + 0x200; property(&w, "table", &runtime, 8);
    node(&w, "kernel-compatibility", 1, 0);
    uint32_t supported = 1; property(&w, "x86_64", &supported, 4);
    clear(entropy, sizeof(entropy));
    if (w.failed) return EFI_BUFFER_TOO_SMALL;
    arguments->deviceTreeP = TREE_BASE; arguments->deviceTreeLength = (uint32_t)w.used;
    return EFI_SUCCESS;
}

static EFI_STATUS copy_tables(void) {
    if (firmware->Hdr.HeaderSize != sizeof(*firmware) ||
        firmware->RuntimeServices->Hdr.HeaderSize > sizeof(EFI_RUNTIME_SERVICES) ||
        firmware->NumberOfTableEntries > (0x4000 - 0x400) / sizeof(EFI_CONFIGURATION_TABLE)) return EFI_UNSUPPORTED;
    EFI_SYSTEM_TABLE *system = (void *)(uintptr_t)TABLE_BASE;
    copy(system, firmware, sizeof(*system));
    copy((void *)(uintptr_t)(TABLE_BASE + 0x200), firmware->RuntimeServices, sizeof(EFI_RUNTIME_SERVICES));
    copy((void *)(uintptr_t)(TABLE_BASE + 0x400), firmware->ConfigurationTable,
         firmware->NumberOfTableEntries * sizeof(EFI_CONFIGURATION_TABLE));
    system->ConfigurationTable = (void *)(uintptr_t)(TABLE_BASE + 0x400);
    system->RuntimeServices = (void *)(uintptr_t)(KERNEL_ALIAS | (TABLE_BASE + 0x200));
    system->BootServices = NULL;
    system->Hdr.CRC32 = 0;
    EFI_STATUS status = FW(services->CalculateCrc32, 3, system, system->Hdr.HeaderSize, &system->Hdr.CRC32);
    arguments->efiSystemTable = TABLE_BASE;
    return status;
}

static int overlap(uint64_t a, uint64_t bytes, uint64_t b, uint64_t other_bytes) {
    return a < b + other_bytes && b < a + bytes;
}
static EFI_STATUS exit_firmware(EFI_HANDLE image) {
    UINTN size = 0, key = 0, stride = 0; UINT32 version = 0;
    EFI_MEMORY_DESCRIPTOR *map = (void *)(uintptr_t)MAP_BASE;
    EFI_STATUS status = EFI_INVALID_PARAMETER;
    for (unsigned int attempt = 0; attempt < 8; ++attempt) {
        size = MAP_CAPACITY;
        status = FW(services->GetMemoryMap, 5, &size, map, &key, &stride, &version);
        if (EFI_ERROR(status)) return status;
        if (stride < sizeof(*map) || stride > 512 || size % stride) return EFI_COMPROMISED_DATA;
        uint64_t covered = 0, ram = 0;
        for (UINTN offset = 0; offset < size; offset += stride) {
            EFI_MEMORY_DESCRIPTOR *d = (void *)((uint8_t *)map + offset);
            if (d->NumberOfPages > (UINT64_MAX - d->PhysicalStart) / 4096) return EFI_COMPROMISED_DATA;
            uint64_t bytes = d->NumberOfPages * 4096;
            if (overlap(KERNEL_BASE, collection.size, d->PhysicalStart, bytes)) {
                if ((d->Attribute & EFI_MEMORY_RUNTIME) ||
                    !((d->Type >= EfiLoaderCode && d->Type <= EfiBootServicesData) ||
                      d->Type == EfiConventionalMemory)) {
                    serial("PC UEFI: kernel destination overlaps firmware type "); hex(d->Type);
                    serial(" at "); hex(d->PhysicalStart); serial(" bytes "); hex(bytes); serial("\r\n");
                    return EFI_UNSUPPORTED;
                }
                uint64_t low = d->PhysicalStart > KERNEL_BASE ? d->PhysicalStart : KERNEL_BASE;
                uint64_t high = d->PhysicalStart + bytes < KERNEL_BASE + collection.size ?
                                d->PhysicalStart + bytes : KERNEL_BASE + collection.size;
                if (high - low > collection.size - covered) return EFI_COMPROMISED_DATA;
                covered += high - low;
            }
            if (d->Type >= EfiLoaderCode && d->Type <= EfiACPIMemoryNVS && d->Type != EfiUnusableMemory) {
                if (bytes > UINT64_MAX - ram) return EFI_COMPROMISED_DATA;
                ram += bytes;
            }
        }
        if (covered != collection.size) {
            serial("PC UEFI: kernel destination RAM coverage "); hex(covered);
            serial(" required "); hex(collection.size); serial("\r\n");
            return EFI_UNSUPPORTED;
        }
        arguments->PhysicalMemorySize = ram;
        // No boot-service allocations or console output may invalidate this key.
        status = FW(services->ExitBootServices, 2, image, key);
        if (!EFI_ERROR(status)) break;
        if (status != EFI_INVALID_PARAMETER) return status;
    }
    if (EFI_ERROR(status)) return status;
    __asm__ volatile("cli" ::: "memory");
    // Sort whole descriptors, including firmware-defined trailing fields.
    uint8_t scratch[512];
    for (UINTN at = stride; at < size; at += stride) {
        copy(scratch, (uint8_t *)map + at, stride);
        UINTN position = at;
        while (position && ((EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + position - stride))->PhysicalStart >
                            ((EFI_MEMORY_DESCRIPTOR *)scratch)->PhysicalStart) {
            copy((uint8_t *)map + position, (uint8_t *)map + position - stride, stride); position -= stride;
        }
        copy((uint8_t *)map + position, scratch, stride);
    }
    uint64_t cursor = RUNTIME_BASE;
    for (UINTN offset = 0; offset < size; offset += stride) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)((uint8_t *)map + offset);
        if (!(d->Attribute & EFI_MEMORY_RUNTIME)) continue;
        uint64_t bytes = d->NumberOfPages * 4096;
        if (bytes > 0x3e800000 - cursor) stop("runtime window", EFI_BAD_BUFFER_SIZE);
        d->VirtualStart = KERNEL_ALIAS | cursor; cursor += bytes;
    }
    arguments->ksize = (uint32_t)(((cursor + 0x1fffff) & ~0x1fffffULL) - KERNEL_BASE);
    arguments->efiRuntimeServicesVirtualPageStart = (KERNEL_ALIAS | RUNTIME_BASE) / 4096;
    EFI_RUNTIME_SERVICES *runtime = firmware->RuntimeServices;
    status = FW(runtime->SetVirtualAddressMap, 4, size, stride, version, map);
    if (EFI_ERROR(status)) stop("SetVirtualAddressMap", status);
    copy((void *)(uintptr_t)(TABLE_BASE + 0x200), runtime, sizeof(*runtime));
    // Reclaim boot-service memory and coalesce adjacent equal descriptors.
    UINTN count = 0;
    for (UINTN offset = 0; offset < size; offset += stride) {
        EFI_MEMORY_DESCRIPTOR *current = (void *)((uint8_t *)map + offset);
        if (current->Type >= EfiLoaderCode && current->Type <= EfiBootServicesData)
            current->Type = EfiConventionalMemory;
        EFI_MEMORY_DESCRIPTOR *previous = count ? (void *)((uint8_t *)map + (count - 1) * stride) : NULL;
        if (previous && !(current->Attribute & EFI_MEMORY_RUNTIME) && previous->Type == current->Type &&
            previous->Attribute == current->Attribute &&
            previous->PhysicalStart + previous->NumberOfPages * 4096 == current->PhysicalStart)
            previous->NumberOfPages += current->NumberOfPages;
        else { copy((uint8_t *)map + count * stride, current, stride); ++count; }
    }
    if (count > 128) stop("XNU memory-region limit", EFI_BAD_BUFFER_SIZE);
    arguments->MemoryMap = MAP_BASE; arguments->MemoryMapSize = (uint32_t)(count * stride);
    arguments->MemoryMapDescriptorSize = (uint32_t)stride; arguments->MemoryMapDescriptorVersion = version;
    serial("PC UEFI: entering native 64-bit Darwin collection\r\n");
    pc_enter_on_private_stack(staging, collection.size, KERNEL_BASE + collection.entry,
                              BOOT_BASE, stack_top);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    InitializeLib(image, system); firmware = system; services = system->BootServices;
    out8(0x3f9, 0); out8(0x3fb, 0x80); out8(0x3f8, 1); out8(0x3f9, 0);
    out8(0x3fb, 3); out8(0x3fa, 0xc7); out8(0x3fc, 3);
    serial("PC UEFI: x86_64 embedded Darwin boot\r\n");
    // The kernel contract uses its four-level bootstrap page tables. Do not
    // enter it from firmware using five-level paging: CR4.LA57 cannot change
    // while paging stays enabled, and this path deliberately never disables it.
    uint64_t firmware_cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(firmware_cr4));
    if (firmware_cr4 & (1ULL << 12))
        return failure("five-level firmware paging", EFI_UNSUPPORTED);
    EFI_GUID image_guid = LOADED_IMAGE_PROTOCOL, filesystem_guid = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_STATUS status = FW(services->HandleProtocol, 3, image, &image_guid, (void **)&loaded);
    if (EFI_ERROR(status)) return failure("loaded image", status);
    EFI_FILE_IO_INTERFACE *filesystem = NULL;
    status = FW(services->HandleProtocol, 3, loaded->DeviceHandle, &filesystem_guid, (void **)&filesystem);
    if (EFI_ERROR(status)) return failure("boot filesystem", status);
    status = FW(filesystem->OpenVolume, 2, filesystem, &volume);
    if (EFI_ERROR(status)) return failure("boot volume", status);
    // One fixed low boot block is covered by XNU's physfree reservation.
    EFI_PHYSICAL_ADDRESS boot = BOOT_BASE;
    status = FW(services->AllocatePages, 4, AllocateAddress, EfiLoaderData, 32, &boot);
    if (EFI_ERROR(status)) return failure("boot block", status);
    clear((void *)(uintptr_t)BOOT_BASE, 32 * 4096);
    arguments->Version = kBootArgsVersion; arguments->Revision = kBootArgsRevision1;
    arguments->efiMode = kBootArgsEfiMode64; arguments->kaddr = KERNEL_BASE;
    status = seed_boot(); if (EFI_ERROR(status)) return failure("fresh entropy", status);
    status = boot_command_line(); if (EFI_ERROR(status)) return failure("command line", status);
    void *file = NULL; UINTN size = 0;
    status = read_file(L"\\EFI\\BOOT\\kernel", 128 * 1024 * 1024, &file, &size);
    if (EFI_ERROR(status)) return failure("kernel file", status);
    if (!pc_image_parse(file, size, &collection)) return failure("kernel collection validation", EFI_LOAD_ERROR);
    if (collection.base != (KERNEL_ALIAS | KERNEL_BASE) || collection.size > BOOT_BASE - KERNEL_BASE)
        return failure("kernel physical link layout", EFI_UNSUPPORTED);
    if (overlap(KERNEL_BASE, collection.size, (uintptr_t)loaded->ImageBase, loaded->ImageSize))
        return failure("loader overlaps kernel destination", EFI_OUT_OF_RESOURCES);
    // Claim the exact link range while firmware allocation records are still
    // authoritative. This excludes live firmware page tables and other owners;
    // the later C copy must never reclaim memory that the firmware still uses.
    EFI_PHYSICAL_ADDRESS destination = KERNEL_BASE;
    status = FW(services->AllocatePages, 4, AllocateAddress, EfiLoaderData,
                (collection.size + 4095) / 4096, &destination);
    if (EFI_ERROR(status)) return failure("kernel destination reservation", status);
    status = pages(collection.size, EfiLoaderData, &staging);
    if (EFI_ERROR(status)) return failure("kernel staging", status);
    if (staging < BOOT_BASE + 32 * 4096) return failure("staging overlaps boot block", EFI_OUT_OF_RESOURCES);
    pc_image_load(&collection, (void *)(uintptr_t)staging);
    arguments->KC_hdrs_vaddr = KERNEL_BASE + collection.header_address;
    FW(services->FreePool, 1, file);
    // The parser retains command pointers into the file; loading is complete
    // before freeing it. Handoff uses only the scalar image extents.
    status = read_file(L"\\ramdisk.img", 128 * 1024 * 1024, &file, &size);
    if (EFI_ERROR(status)) return failure("HFS RAM disk", status);
    if (size != 64 * 1024 * 1024 && size != 128 * 1024 * 1024)
        return failure("RAM-disk size", EFI_BAD_BUFFER_SIZE);
    if (!equal((uint8_t *)file + 1024, "H+", 2) && !equal((uint8_t *)file + 1024, "HX", 2))
        return failure("HFS+ volume header", EFI_UNSUPPORTED);
    ramdisk_size = size;
    status = pages(size, EfiReservedMemoryType, &ramdisk_base);
    if (EFI_ERROR(status)) return failure("RAM-disk reservation", status);
    if (ramdisk_base < BOOT_BASE + 32 * 4096) return failure("RAM-disk location", EFI_OUT_OF_RESOURCES);
    copy((void *)(uintptr_t)ramdisk_base, file, size); FW(services->FreePool, 1, file);
    status = pages(65536, EfiLoaderData, &stack_top);
    if (EFI_ERROR(status)) return failure("handoff stack", status);
    if (stack_top < BOOT_BASE + 32 * 4096) return failure("stack location", EFI_OUT_OF_RESOURCES);
    stack_top += 65536 - 16;
    clock_frequency();
    status = video(); if (EFI_ERROR(status)) return failure("GOP framebuffer", status);
    status = copy_tables(); if (EFI_ERROR(status)) return failure("EFI table handoff", status);
    status = device_tree(); if (EFI_ERROR(status)) return failure("device tree", status);
    FW(volume->Close, 1, volume);
    status = FW(services->SetWatchdogTimer, 4, 0, 0, 0, NULL);
    if (EFI_ERROR(status)) return failure("watchdog", status);
    status = exit_firmware(image);
    // Firmware console calls are not permitted after an ExitBootServices attempt.
    stop("firmware handoff", status);
}
