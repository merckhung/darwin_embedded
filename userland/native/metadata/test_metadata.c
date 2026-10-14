/* Compare native loader APIs with the actual loaded Mach-O metadata. */
#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>

_Static_assert(sizeof(dyld_build_version_t) == 8, "dyld1378 SDK token fields");
_Static_assert(offsetof(struct dyld_image_uuid_offset, offsetInImage) == 16 &&
               offsetof(struct dyld_image_uuid_offset, image) == 24 &&
               sizeof(struct dyld_image_uuid_offset) == 32, "native UUID/address ABI");

int native_metadata_check(void);
static const struct mach_header *loaded_headers[128];
static const char *loaded_paths[128];
static unsigned loaded_count;
static int invalid_callback;
static int mutable_value = 7;
/* This fixture remains addressable, requiring a relocation in DATA_CONST. */
__attribute__((used, section("__DATA_CONST,__const")))
static int *const constant_pointer = &mutable_value;

static int equal(const char *left, const char *right)
{
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static int bytes_equal(const void *left, const void *right, size_t size)
{
    const unsigned char *a = left, *b = right;
    for (size_t n = 0; n < size; ++n)
        if (a[n] != b[n]) return 0;
    return 1;
}

static void capture(unsigned count, const struct mach_header *headers[], const char *paths[])
{
    if (loaded_count || count > 128) { invalid_callback = 1; return; }
    loaded_count = count;
    for (unsigned n = 0; n < count; ++n) {
        loaded_headers[n] = headers[n];
        loaded_paths[n] = paths[n];
        if (!headers[n] || !paths[n] || paths[n][0] != '/') invalid_callback = 1;
    }
}

struct image_view {
    const struct mach_header_64 *header;
    const struct segment_command_64 *text;
    const struct section_64 *text_section;
    const struct uuid_command *uuid;
    uint32_t platform, minimum_os, sdk;
    uintptr_t slide;
};

static int inspect(const struct mach_header *header, struct image_view *view)
{
    if (!header || header->magic != MH_MAGIC_64) return 0;
    *view = (struct image_view){.header = (const void *)header};
    const unsigned char *cursor = (const void *)(view->header + 1);
    const unsigned char *end = cursor + header->sizeofcmds;
    int found_slide = 0;
    for (unsigned n = 0; n < header->ncmds; ++n) {
        if (cursor > end || (size_t)(end - cursor) < sizeof(struct load_command)) return 0;
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor)) return 0;
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const void *)command;
            if (command->cmdsize < sizeof(*segment) ||
                segment->nsects > (command->cmdsize - sizeof(*segment)) / sizeof(struct section_64)) return 0;
            if (!segment->fileoff && segment->filesize) {
                view->slide = (uintptr_t)header - segment->vmaddr;
                found_slide = 1;
            }
            if (bytes_equal(segment->segname, "__TEXT\0", 7)) {
                view->text = segment;
                const struct section_64 *sections = (const void *)(segment + 1);
                for (unsigned j = 0; j < segment->nsects; ++j)
                    if (bytes_equal(sections[j].sectname, "__text\0", 7)) view->text_section = sections + j;
            }
        } else if (command->cmd == LC_BUILD_VERSION) {
            if (command->cmdsize < sizeof(struct build_version_command)) return 0;
            const struct build_version_command *version = (const void *)command;
            view->platform = version->platform;
            view->minimum_os = version->minos;
            view->sdk = version->sdk;
        } else if (command->cmd == LC_UUID) {
            if (command->cmdsize < sizeof(struct uuid_command)) return 0;
            view->uuid = (const void *)command;
        }
        cursor += command->cmdsize;
    }
    return cursor == end && found_slide && view->text &&
           view->platform == PLATFORM_MACOS && view->sdk;
}

static int uuid_matches(const struct mach_header *header, const struct image_view *view)
{
    uuid_t value;
    for (unsigned n = 0; n < sizeof(value); ++n) value[n] = 0x55;
    int found = _dyld_get_image_uuid(header, value);
    if (view->uuid) return found && bytes_equal(value, view->uuid->uuid, sizeof(value));
    if (found) return 0;
    for (unsigned n = 0; n < sizeof(value); ++n) if (value[n]) return 0;
    return 1;
}

static int link_versions(const struct image_view *main)
{
    const unsigned char *cursor = (const void *)(main->header + 1);
    unsigned checked = 0;
    for (unsigned n = 0; n < main->header->ncmds; ++n) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_LOAD_DYLIB || command->cmd == LC_LOAD_WEAK_DYLIB ||
            command->cmd == LC_REEXPORT_DYLIB || command->cmd == LC_LOAD_UPWARD_DYLIB) {
            if (command->cmdsize < sizeof(struct dylib_command)) return 0;
            const struct dylib_command *library = (const void *)command;
            if (library->dylib.name.offset >= command->cmdsize) return 0;
            const char *path = (const char *)command + library->dylib.name.offset;
            const char *leaf = path;
            const char *limit = (const char *)command + command->cmdsize;
            const char *p;
            for (p = path; p < limit && *p; ++p) if (*p == '/') leaf = p + 1;
            if (p == limit || leaf == p) return 0;
            char name[128];
            if (p - leaf > 3 && leaf[0] == 'l' && leaf[1] == 'i' && leaf[2] == 'b') leaf += 3;
            unsigned length = 0;
            while (leaf < p && *leaf != '.') {
                if (length + 1 == sizeof(name)) return 0;
                name[length++] = *leaf++;
            }
            name[length] = 0;
            if (NSVersionOfLinkTimeLibrary(name) != (int32_t)library->dylib.current_version) return 0;
            ++checked;
        }
        cursor += command->cmdsize;
    }
    return checked && NSVersionOfLinkTimeLibrary("native_metadata_absent_library_9f3c") == -1;
}

int native_metadata_check(void)
{
    _dyld_register_for_bulk_image_loads(capture);
    if (invalid_callback || loaded_count < 4 || _dyld_get_image_header(loaded_count) ||
        _dyld_get_image_header(UINT32_MAX)) return 1;
    struct image_view main;
    if (!inspect(loaded_headers[0], &main) || main.header->filetype != MH_EXECUTE ||
        !main.uuid || !main.text_section) return 2;
    for (unsigned n = 0; n < loaded_count; ++n) {
        struct image_view view;
        if (_dyld_get_image_header(n) != loaded_headers[n] || !inspect(loaded_headers[n], &view)) return 3;
        if ((uintptr_t)_dyld_get_image_slide(loaded_headers[n]) != view.slide || !uuid_matches(loaded_headers[n], &view)) return 4;
        unsigned long size = 0;
        unsigned char *section = getsectiondata(view.header, "__TEXT", "__text", &size);
        if (view.text_section) {
            if (section != (unsigned char *)(view.text_section->addr + view.slide) ||
                size != view.text_section->size || !size) return 5;
        } else if (section || size) return 5;
        if (!dyld_sdk_at_least(loaded_headers[n], (dyld_build_version_t){view.platform, view.sdk}) ||
            dyld_sdk_at_least(loaded_headers[n], (dyld_build_version_t){view.platform, view.sdk + 1}) ||
            dyld_sdk_at_least(loaded_headers[n], (dyld_build_version_t){PLATFORM_IOS, 0})) return 6;
    }
    if (dyld_get_active_platform() != main.platform ||
        !dyld_program_sdk_at_least((dyld_build_version_t){main.platform, main.sdk}) ||
        dyld_program_sdk_at_least((dyld_build_version_t){main.platform, main.sdk + 1}) ||
        dyld_program_sdk_at_least((dyld_build_version_t){PLATFORM_IOS, 0}) ||
        !dyld_program_sdk_at_least(dyld_fall_2018_os_versions) ||
        !dyld_sdk_at_least(loaded_headers[0], dyld_fall_2018_os_versions) ||
        dyld_program_sdk_at_least((dyld_build_version_t){UINT32_MAX, UINT32_MAX}) ||
        dyld_sdk_at_least(loaded_headers[0], (dyld_build_version_t){UINT32_MAX, UINT32_MAX})) return 7;
    /* Exact dyld1378 DyldAPIs.cpp token layout; not a cross-release promise. */
    union { uint64_t value; dyld_build_version_t fields; } token = {
        .fields = {main.platform, main.sdk}
    };
    if (dyld_get_program_sdk_version_token() != token.value ||
        dyld_get_program_min_os_version() != main.minimum_os || !link_versions(&main)) return 8;
    const void *self = (const void *)(uintptr_t)&native_metadata_check;
    const struct mach_header *own = dyld_image_header_containing_address(self);
    struct image_view own_view;
    if (!inspect(own, &own_view)) return 9;
    Dl_info info;
    if (!dladdr((const unsigned char *)self + 1, &info) || info.dli_fbase != (void *)own ||
        !info.dli_fname || !info.dli_sname || !equal(info.dli_sname, "native_metadata_check") ||
        info.dli_saddr != self) return 10;
    unsigned own_index = 0;
    while (own_index < loaded_count && loaded_headers[own_index] != own) ++own_index;
    if (own_index == loaded_count || !equal(info.dli_fname, loaded_paths[own_index])) return 11;
    if (!dladdr(own, &info) || info.dli_fbase != (void *)own || info.dli_saddr != (void *)own ||
        !info.dli_sname || !equal(info.dli_sname, "__dso_handle")) return 12;
    const void *loader_function = (const void *)(uintptr_t)&_dyld_get_image_header;
    const struct mach_header *loader = dyld_image_header_containing_address(loader_function);
    if (!loader || loader->filetype != MH_DYLINKER || !dladdr(loader_function, &info) ||
        info.dli_fbase != (void *)loader || !info.dli_fname || !equal(info.dli_fname, "/usr/lib/dyld")) return 13;
    int stack_value = 1;
    const void *addresses[] = {self, main.header, &stack_value, (const void *)UINTPTR_MAX};
    struct dyld_image_uuid_offset offsets[4];
    _dyld_images_for_addresses(4, addresses, offsets);
    if (offsets[0].image != own || offsets[0].offsetInImage != (uintptr_t)self - (uintptr_t)own ||
        !own_view.uuid || !bytes_equal(offsets[0].uuid, own_view.uuid->uuid, sizeof(uuid_t)) ||
        offsets[1].image != loaded_headers[0] || offsets[1].offsetInImage ||
        !bytes_equal(offsets[1].uuid, main.uuid->uuid, sizeof(uuid_t))) return 14;
    for (unsigned n = 2; n < 4; ++n) {
        if (offsets[n].image || offsets[n].offsetInImage || dyld_image_header_containing_address(addresses[n])) return 15;
        for (unsigned j = 0; j < sizeof(uuid_t); ++j) if (offsets[n].uuid[j]) return 16;
    }
    if (dladdr(&stack_value, &info) || info.dli_fname || info.dli_fbase || info.dli_sname || info.dli_saddr) return 17;
    if ((own_view.text->maxprot & VM_PROT_WRITE) || !_dyld_is_memory_immutable(self, 1) ||
        _dyld_is_memory_immutable(&stack_value, sizeof(stack_value)) ||
        _dyld_is_memory_immutable(&mutable_value, sizeof(mutable_value)) ||
        _dyld_is_memory_immutable(&constant_pointer, sizeof(constant_pointer)) || constant_pointer != &mutable_value ||
        _dyld_is_memory_immutable(self, 0) || _dyld_is_memory_immutable(self, SIZE_MAX) ||
        _dyld_is_memory_immutable((const void *)UINTPTR_MAX, 2) ||
        _dyld_is_memory_immutable((const void *)(own_view.text->vmaddr + own_view.slide + own_view.text->vmsize - 1), 2)) return 18;
    const unsigned char *cursor = (const void *)(own_view.header + 1);
    int data_const_checked = 0;
    for (unsigned n = 0; n < own_view.header->ncmds; ++n) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const void *)command;
            uintptr_t start = segment->vmaddr + own_view.slide;
            if ((uintptr_t)&constant_pointer >= start && (uintptr_t)&constant_pointer - start < segment->vmsize) {
                if (!bytes_equal(segment->segname, "__DATA_CONST\0", 13) || !(segment->maxprot & VM_PROT_WRITE)) return 19;
                data_const_checked = 1;
            }
        }
        cursor += command->cmdsize;
    }
    if (!data_const_checked) return 20;
    const void *bottom = self, *top = self;
    _dyld_stack_range(&bottom, &top);
    size_t cache_size = SIZE_MAX;
    uuid_t cache_uuid;
    for (unsigned n = 0; n < sizeof(cache_uuid); ++n) cache_uuid[n] = 0x5a;
    if (bottom || top || _dyld_get_shared_cache_range(&cache_size) || cache_size ||
        _dyld_get_shared_cache_uuid(cache_uuid)) return 21;
    for (unsigned n = 0; n < sizeof(cache_uuid); ++n) if (cache_uuid[n] != 0x5a) return 22;
    if (!dyld_process_is_restricted()) return 23;
    struct mach_header_64 unknown = {.magic = MH_MAGIC_64};
    uuid_t unknown_uuid;
    if (_dyld_get_image_uuid((const void *)&unknown, unknown_uuid) ||
        _dyld_get_image_slide((const void *)&unknown) ||
        dyld_sdk_at_least((const void *)&unknown, (dyld_build_version_t){PLATFORM_MACOS, 0})) return 24;
    for (unsigned n = 0; n < sizeof(unknown_uuid); ++n) if (unknown_uuid[n]) return 25;
    return 0;
}
