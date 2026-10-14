// Native loader integration around exact Apple dyld TLS methods. The loader
// validates mapped sections before notification and pins images with TLS.
extern "C" void _tlv_bootstrap(void);

namespace {
struct RegisteredTLSImage {
    const mach_header* header;
    RegisteredTLSImage* next;
};
RegisteredTLSImage* registeredTLSImages;
pthread_mutex_t registrationLock = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t registrationOnce = PTHREAD_ONCE_INIT;

void validateTLSImage(const mach_o::UnsafeHeader* image)
{
    // The upstream private method assumes a loader-validated, linker-produced
    // image. Check that boundary explicitly before modifying any descriptors.
    __block uint64_t begin = 0;
    __block uint64_t end = 0;
    __block uint32_t templateSegment = 0;
    __block bool templateFound = false;
    __block bool initializedContent = false;
    __block size_t descriptorCount = 0;
    image->forEachSection(^(const mach_o::UnsafeHeader::SectionInfo& section, bool&) {
        uint32_t type = section.flags & SECTION_TYPE;
        if (type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL) {
            if (!section.size)
                return;
            if (section.size > UINT64_MAX - section.address ||
                (templateFound && (section.address < end || templateSegment != section.segIndex)))
                abort_report_np("native TLS template ranges are not ordered in one segment");
            if (!templateFound) {
                begin = section.address;
                templateSegment = section.segIndex;
                templateFound = true;
            }
            end = section.address + section.size;
            initializedContent |= type == S_THREAD_LOCAL_REGULAR;
            if (!(section.segInitProt & VM_PROT_READ))
                abort_report_np("native TLS template segment is unreadable");
        }
        if (type == S_THREAD_LOCAL_VARIABLES) {
            if (!(section.segInitProt & VM_PROT_WRITE) || section.size % sizeof(dyld::ThreadLocalVariables::Thunk))
                abort_report_np("native TLS descriptor section is not writable or has partial entries");
            descriptorCount += section.size / sizeof(dyld::ThreadLocalVariables::Thunk);
        }
        if (type == S_THREAD_LOCAL_INIT_FUNCTION_POINTERS && section.size)
            abort_report_np("native TLS initializer-pointer sections are not implemented by dyld1378 disk TLS setup");
    });
    if (!templateFound || !descriptorCount || end - begin > UINT32_MAX)
        abort_report_np("native TLS image has no usable template or descriptors");
    uint64_t totalSize = end - begin;
    uintptr_t slide = image->getSlide();
    uintptr_t templateAddress = begin + slide;
    image->forEachSection(^(const mach_o::UnsafeHeader::SectionInfo& section, bool&) {
        if ((section.flags & SECTION_TYPE) != S_THREAD_LOCAL_VARIABLES)
            return;
        auto* thunks = reinterpret_cast<dyld::ThreadLocalVariables::Thunk*>(section.address + slide);
        for (size_t n = 0; n != section.size / sizeof(*thunks); ++n) {
            if (thunks[n].key != 0 || thunks[n].offset >= totalSize || thunks[n].func != (void*)&_tlv_bootstrap)
                abort_report_np("native TLS descriptor is unbound, already initialized, or out of range");
            uintptr_t deltaField = reinterpret_cast<uintptr_t>(&thunks[n].offset);
            if (initializedContent &&
                ((templateAddress >= deltaField && templateAddress - deltaField > INT32_MAX) ||
                 (templateAddress < deltaField && deltaField - templateAddress > (uint64_t)INT32_MAX + 1)))
                abort_report_np("native TLS template delta exceeds the dyld1378 descriptor format");
        }
    });
}

void registerTLSImages(unsigned count, const mach_header* headers[], const char* paths[])
{
    (void)paths;
    pthread_mutex_lock(&registrationLock);
    for (unsigned index = 0; index != count; ++index) {
        const mach_header* header = headers[index];
        if (!(header->flags & MH_HAS_TLV_DESCRIPTORS))
            continue;
        bool known = false;
        for (RegisteredTLSImage* entry = registeredTLSImages; entry; entry = entry->next)
            if (entry->header == header)
                known = true;
        if (known)
            continue;
        if (header->magic != MH_MAGIC_64 || (header->flags & MH_DYLIB_IN_CACHE))
            abort_report_np("native TLS requires a validated 64-bit disk image");
        const mach_o::UnsafeHeader* image = reinterpret_cast<const mach_o::UnsafeHeader*>(header);
        validateTLSImage(image);
        if (mach_o::Error error = dyld::sThreadLocalVariables.initializeThunksFromDisk(image))
            abort_report_np("native TLS setup: %s", error.message());
        RegisteredTLSImage* entry = static_cast<RegisteredTLSImage*>(malloc(sizeof(RegisteredTLSImage)));
        if (!entry)
            abort_report_np("native TLS image registration allocation failed");
        *entry = {header, registeredTLSImages};
        registeredTLSImages = entry;
    }
    pthread_mutex_unlock(&registrationLock);
}

void registerTLSCallback()
{
    _dyld_register_for_bulk_image_loads(registerTLSImages);
}
}

extern "C" void native_tls_init()
{
    // Must run after real pthread, malloc, and native_tlv_init initialization.
    pthread_once(&registrationOnce, registerTLSCallback);
}
