/* Native Mach-O dynamic linker for the source-built embedded Darwin userland.
 * Uses Apple's BSD/Mach entry points and the actual Mach-O rebase/bind ABI.
 * Libraries are file-mapped after F_ADDFILESIGS registers their CodeDirectory.
 * The initial build uses classic dyld opcodes (-no_fixup_chains).
 */
#include "native_calls.h"
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include "native_version_map.h"
#include "native_codesign_constants.h"
#ifndef NATIVE_DYLD_HOST_TEST
#include <os/tsd.h>
#endif

#if defined(__arm64__)
#define PAGE_SIZE 16384UL
#define NATIVE_CPU 0x0100000c
#else
#define PAGE_SIZE 4096UL
#define NATIVE_CPU 0x01000007
#endif
#define MAX_IMAGES 128
#define MAX_SEGMENTS 32
#define MAX_DEPS 64
#define MAX_PATH 1024
#define MAX_COMMANDS 32768
#include "dynamic.h"

struct image {
    const struct mach_header_64 *header;
    const struct segment_command_64 *segments[MAX_SEGMENTS];
    const struct dyld_info_command *info;
    const struct symtab_command *symbols;
    const struct linkedit_data_command *signature;
    const struct uuid_command *uuid;
    struct image *dependencies[MAX_DEPS];
    unsigned char reexports[MAX_DEPS];
    unsigned segment_count, dependency_count, initialized;
    uintptr_t preferred, slide;
    uint64_t entry_offset;
    uint32_t platform, minimum_os, sdk;
    void *reservation;
    size_t reservation_size;
    int descriptor;
    unsigned permanent, global, open_count, pins, unloading, marked;
    uint64_t initializing_thread;
    uint64_t loading_thread;
    unsigned initialization_order;
    uint64_t load_order;
    struct image *binding_dependencies[MAX_IMAGES];
    unsigned binding_dependency_count;
    struct image *load_parent;
    char path[MAX_PATH];
};
struct stream { const unsigned char *cursor, *end; };
static struct image images[MAX_IMAGES];
static struct image linker_image;
static unsigned image_count;
static unsigned initialization_sequence;
static uint64_t load_sequence;
static void *bootstrap_tsd[64] __attribute__((aligned(16)));
static int bootstrap_errno;
extern const struct mach_header_64 _mh_dylinker_header;
typedef void (*bulk_image_callback)(unsigned, const struct mach_header *[], const char *[]);
static bulk_image_callback image_callbacks[64];
static unsigned image_callback_count;
typedef void (*remove_image_callback)(const struct mach_header *, intptr_t);
static remove_image_callback remove_image_callbacks[64];
static unsigned remove_image_callback_count;
static struct {
    uint32_t code_signing_flags;
    unsigned queried, code_signing_known, changed_credentials, restrict_section;
    unsigned environment_paths_allowed;
} security_policy;

static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static int same(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; } return *a == *b;
}
static void message(const char *s) {
    if (__write_nocancel(2, s, length(s)) >= 0) return;
    /* PID 1 starts before standard descriptors are opened by launchd. */
    int console = __open_nocancel("/dev/console", 1, 0);
    if (console >= 0) {
        __write_nocancel(console, s, length(s));
        __close_nocancel(console);
    }
}
static _Noreturn void fail(const char *reason, const char *detail) {
    if (active_transaction && loader_lock_owner == native_thread_identity()) {
        error_text(active_transaction->error, reason, detail);
        _longjmp(active_transaction->recovery, 1);
    }
    message("native dyld: "); message(reason);
    if (detail) { message(": "); message(detail); }
    message("\n"); __exit(78);
}
static void require(int condition, const char *reason) { if (!condition) fail(reason, 0); }
static uint64_t rounded(uint64_t n) {
    require(n <= UINT64_MAX - PAGE_SIZE + 1, "page size overflow");
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}
static void copy_path(char *out, const char *source) {
    size_t n = length(source);
    require(n < MAX_PATH, "library path too long");
    memcpy(out, source, n + 1);
}
static const char *command_string(const struct load_command *command, uint32_t offset) {
    require(offset >= 8 && offset < command->cmdsize, "invalid load-command string");
    const char *s = (const void *)((const unsigned char *)command + offset);
    for (uint32_t n = offset; n < command->cmdsize; ++n)
        if (!((const unsigned char *)command)[n]) return s;
    fail("unterminated load-command string", 0);
}

static void parse_image(struct image *image, const struct mach_header_64 *header, uint32_t kind) {
    require(header->magic == MH_MAGIC_64 && header->cputype == NATIVE_CPU &&
            header->filetype == kind, "foreign or invalid Mach-O image");
    require(header->ncmds && header->ncmds <= 256 && header->sizeofcmds <= MAX_COMMANDS,
            "invalid Mach-O command area");
    image->header = header;
    image->preferred = UINTPTR_MAX;
    image->entry_offset = UINT64_MAX;
    const unsigned char *commands = (const void *)(header + 1);
    uint32_t cursor = 0;
    for (unsigned n = 0; n < header->ncmds; ++n) {
        require(cursor <= header->sizeofcmds && header->sizeofcmds - cursor >= 8,
                "truncated load command");
        const struct load_command *command = (const void *)(commands + cursor);
        require(command->cmdsize >= 8 && !(command->cmdsize & 7) &&
                command->cmdsize <= header->sizeofcmds - cursor, "invalid load-command size");
        switch (command->cmd) {
        case LC_SEGMENT_64: {
            require(command->cmdsize >= sizeof(struct segment_command_64), "truncated segment");
            const struct segment_command_64 *s = (const void *)command;
            require(s->nsects <= (command->cmdsize - sizeof(*s)) / sizeof(struct section_64),
                    "truncated section table");
            require(s->filesize <= s->vmsize && s->vmaddr <= UINT64_MAX - s->vmsize &&
                    s->fileoff <= UINT64_MAX - s->filesize, "invalid segment range");
            require(!((s->maxprot | s->initprot) & ~7) &&
                    !(s->initprot & ~s->maxprot) && (s->initprot & 6) != 6,
                    "invalid native segment protection");
            require(!(s->vmaddr & (PAGE_SIZE - 1)) && !(s->fileoff & (PAGE_SIZE - 1)),
                    "unaligned Mach-O segment");
            require(s->vmaddr <= UINT64_MAX - rounded(s->vmsize), "rounded segment overflow");
            const struct section_64 *sections = (const void *)(s + 1);
            for (unsigned j = 0; j < s->nsects; ++j)
                require(sections[j].addr >= s->vmaddr && sections[j].size <= s->vmsize &&
                        sections[j].addr - s->vmaddr <= s->vmsize - sections[j].size,
                        "native section is outside its mapping");
            for (unsigned j = 0; j < image->segment_count; ++j) {
                const struct segment_command_64 *other = image->segments[j];
                if (s->vmsize && other->vmsize)
                    require(s->vmaddr >= other->vmaddr + rounded(other->vmsize) ||
                            other->vmaddr >= s->vmaddr + rounded(s->vmsize),
                            "overlapping native segments");
            }
            require(image->segment_count < MAX_SEGMENTS, "too many Mach-O segments");
            image->segments[image->segment_count++] = s;
            if (!s->fileoff && s->filesize) {
                require(image->preferred == UINTPTR_MAX &&
                        s->filesize >= sizeof(*header) + header->sizeofcmds, "invalid header segment");
                image->preferred = s->vmaddr;
            }
            break;
        }
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
            require(command->cmdsize == sizeof(struct dyld_info_command) && !image->info,
                    "invalid dyld info");
            image->info = (const void *)command;
            break;
        case LC_SYMTAB:
            require(command->cmdsize == sizeof(struct symtab_command) && !image->symbols,
                    "invalid symbol table command");
            image->symbols = (const void *)command;
            break;
        case LC_CODE_SIGNATURE:
            require(command->cmdsize == sizeof(struct linkedit_data_command) && !image->signature,
                    "invalid code signature command");
            image->signature = (const void *)command;
            break;
        case LC_MAIN:
            require(command->cmdsize == sizeof(struct entry_point_command) &&
                    image->entry_offset == UINT64_MAX, "invalid entry point command");
            image->entry_offset = ((const struct entry_point_command *)command)->entryoff;
            break;
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB: case LC_ID_DYLIB:
            require(command->cmdsize >= sizeof(struct dylib_command), "truncated dylib command");
            command_string(command, ((const struct dylib_command *)command)->dylib.name.offset);
            break;
        case LC_LOAD_DYLINKER: case LC_ID_DYLINKER: case LC_RPATH:
            require(command->cmdsize >= 12, "truncated pathname command");
            command_string(command, ((const uint32_t *)command)[2]);
            break;
        case LC_DYLD_CHAINED_FIXUPS:
            fail("chained fixups require a native loader extension", image->path);
        case LC_UUID:
            require(command->cmdsize == sizeof(struct uuid_command) && !image->uuid,
                    "invalid native UUID command");
            image->uuid = (const void *)command;
            break;
        case LC_VERSION_MIN_MACOSX: {
            require(command->cmdsize == sizeof(struct version_min_command) && !image->platform,
                    "invalid native minimum-version command");
            const struct version_min_command *version = (const void *)command;
            image->platform = PLATFORM_MACOS;
            image->minimum_os = version->version;
            image->sdk = version->sdk;
            break;
        }
        case LC_BUILD_VERSION: {
            require(command->cmdsize >= sizeof(struct build_version_command) && !image->platform,
                    "invalid native build-version command");
            const struct build_version_command *version = (const void *)command;
            require(version->platform == PLATFORM_MACOS,
                    "unsupported native Mach-O platform");
            require(version->ntools == (command->cmdsize - sizeof(*version)) / sizeof(struct build_tool_version),
                    "invalid native build-tool table");
            image->platform = version->platform;
            image->minimum_os = version->minos;
            image->sdk = version->sdk;
            break;
        }
        case LC_DYSYMTAB: case LC_FUNCTION_STARTS: case LC_DATA_IN_CODE:
        case LC_SOURCE_VERSION: case LC_DYLIB_CODE_SIGN_DRS:
            break;
        case LC_SEGMENT_SPLIT_INFO:
            require(command->cmdsize == sizeof(struct linkedit_data_command),
                    "invalid split-segment metadata");
            break;
        case LC_UNIXTHREAD:
            require(kind == MH_DYLINKER, "unexpected Unix thread command");
            break;
        default: fail("unsupported Mach-O load command", image->path);
        }
        cursor += command->cmdsize;
    }
    require(cursor == header->sizeofcmds && image->preferred != UINTPTR_MAX,
            "missing Mach-O header mapping");
    image->slide = (uintptr_t)header - image->preferred;
}

static const void *file_range(const struct image *image, uint64_t offset, uint64_t size) {
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *s = image->segments[n];
        if (offset >= s->fileoff && offset - s->fileoff <= s->filesize &&
            size <= s->filesize - (offset - s->fileoff))
            return (const void *)(s->vmaddr + image->slide + offset - s->fileoff);
    }
    fail("metadata is outside a file-backed segment", image->path);
}
static uintptr_t *fixup_slot(const struct image *image, unsigned segment, uint64_t offset) {
    require(segment < image->segment_count, "fixup has invalid segment index");
    const struct segment_command_64 *s = image->segments[segment];
    require((s->maxprot & 2) && !(s->initprot & 4) && offset <= s->vmsize &&
            sizeof(uintptr_t) <= s->vmsize - offset && !(offset & 7), "invalid pointer fixup target");
    return (void *)(s->vmaddr + image->slide + offset);
}
static uint64_t uleb(struct stream *stream) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        require(stream->cursor < stream->end, "truncated ULEB128");
        unsigned char byte = *stream->cursor++;
        require(shift != 63 || !(byte & 0xfe), "ULEB128 overflow");
        value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return value;
    }
    fail("ULEB128 overflow", 0);
}
static int64_t sleb(struct stream *stream) {
    uint64_t value = 0; unsigned shift = 0; unsigned char byte;
    do {
        require(stream->cursor < stream->end && shift < 64, "invalid SLEB128");
        byte = *stream->cursor++;
        if (shift == 63) require((byte & 0x7f) == 0 || (byte & 0x7f) == 0x7f, "SLEB128 overflow");
        value |= (uint64_t)(byte & 0x7f) << shift; shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) value |= UINT64_MAX << shift;
    return (int64_t)value;
}
static void advance(uint64_t *offset, uint64_t amount) {
    require(amount <= UINT64_MAX - *offset, "fixup offset overflow");
    *offset += amount;
}
static struct stream metadata(const struct image *image, uint32_t offset, uint32_t size) {
    const unsigned char *p = file_range(image, offset, size);
    return (struct stream){p, p + size};
}

static void rebase(struct image *image) {
    if (!image->info || !image->info->rebase_size) return;
    struct stream stream = metadata(image, image->info->rebase_off, image->info->rebase_size);
    unsigned segment = 0, type = 0; uint64_t offset = 0;
    while (stream.cursor < stream.end) {
        unsigned char byte = *stream.cursor++, opcode = byte & 0xf0, immediate = byte & 0xf;
        uint64_t count = 0, skip = 0;
        switch (opcode) {
        case REBASE_OPCODE_DONE: return;
        case REBASE_OPCODE_SET_TYPE_IMM: type = immediate; continue;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: segment = immediate; offset = uleb(&stream); continue;
        case REBASE_OPCODE_ADD_ADDR_ULEB: advance(&offset, uleb(&stream)); continue;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: advance(&offset, immediate * sizeof(uintptr_t)); continue;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES: count = immediate; break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: count = uleb(&stream); break;
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: count = 1; skip = uleb(&stream); break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: count = uleb(&stream); skip = uleb(&stream); break;
        default: fail("unsupported rebase opcode", image->path);
        }
        require(type == REBASE_TYPE_POINTER && count <= 1000000, "invalid pointer rebase");
        for (uint64_t n = 0; n < count; ++n) {
            uintptr_t *slot = fixup_slot(image, segment, offset);
            *slot += image->slide;
            advance(&offset, sizeof(uintptr_t)); advance(&offset, skip);
        }
    }
    fail("unterminated rebase stream", image->path);
}

static int find_export_detail(struct image *image, const char *name, unsigned depth, uintptr_t *result, int *weak) {
    if (depth >= MAX_IMAGES) return 0;
    if (image->symbols) {
        const struct symtab_command *s = image->symbols;
        const struct nlist_64 *symbols = file_range(image, s->symoff, (uint64_t)s->nsyms * sizeof(*symbols));
        const char *strings = file_range(image, s->stroff, s->strsize);
        for (unsigned n = 0; n < s->nsyms; ++n) {
            const struct nlist_64 *entry = symbols + n;
            if ((entry->n_type & N_STAB) || !(entry->n_type & N_EXT) || (entry->n_type & N_PEXT)) continue;
            unsigned kind = entry->n_type & N_TYPE;
            if (kind != N_SECT && kind != N_ABS) continue;
            require(entry->n_un.n_strx < s->strsize, "symbol name outside string table");
            const char *symbol = strings + entry->n_un.n_strx;
            size_t limit = s->strsize - entry->n_un.n_strx;
            size_t j = 0; while (j < limit && symbol[j]) ++j;
            require(j < limit, "unterminated symbol name");
            if (same(symbol, name)) {
                *result = entry->n_value + (kind == N_SECT ? image->slide : 0);
                if (weak) *weak = (entry->n_desc & N_WEAK_DEF) != 0;
                return 1;
            }
        }
    }
    for (unsigned n = 0; n < image->dependency_count; ++n)
        if (image->reexports[n] && image->dependencies[n]) {
            if (find_export_detail(image->dependencies[n], name, depth + 1, result, weak)) return 1;
        }
    return 0;
}
static int find_export(struct image *image, const char *name, unsigned depth, uintptr_t *result) {
    return find_export_detail(image, name, depth, result, 0);
}
static int find_coalesced(struct image *requester, const char *name, uintptr_t *result) {
    int found = 0, chosen_weak = 1; uint64_t chosen_order = UINT64_MAX;
    /* Native dyld's opcode clients prefer a non-weak definition, otherwise
     * the earliest public weak definition. RTLD_LOCAL roots stay hidden. */
    for (unsigned n = 0; n < image_count; ++n) {
        struct image *candidate = images + n;
        if (!candidate->header || candidate->unloading) continue;
        int visible = candidate->global || (in_transaction(candidate) &&
            !(active_transaction->local && candidate == active_transaction->root));
        if (!visible) continue;
        uintptr_t address; int weak;
        if (!find_export_detail(candidate, name, 0, &address, &weak)) continue;
        if (!found || (chosen_weak && !weak) ||
            (chosen_weak == weak && candidate->load_order < chosen_order)) {
            *result = address; found = 1; chosen_weak = weak; chosen_order = candidate->load_order;
        }
    }
    return found || find_export(requester, name, 0, result);
}
static uintptr_t exported(struct image *image, const char *name, unsigned depth) {
    uintptr_t result = 0;
    find_export(image, name, depth, &result);
    return result;
}
static _Noreturn void unresolved_lazy_symbol(void) { fail("unresolved lazy symbol", 0); }
void _dyld_register_for_bulk_image_loads(bulk_image_callback callback) {
    MUTATION_LOCKED();
    lock_loader();
    require(callback != 0, "null native image callback");
    require(image_callback_count < 64, "too many native image callbacks");
    /* dyld1378 RuntimeState::addNotifyBulkLoadImage retains the callback's
     * owner because the API provides no operation to unregister it. */
    retain_permanently((struct image *)image_for_address((void *)callback));
    image_callbacks[image_callback_count++] = callback;
    const struct mach_header *headers[MAX_IMAGES];
    const char *paths[MAX_IMAGES];
    struct image *snapshot[MAX_IMAGES];
    unsigned count = 0;
    for (unsigned n = 0; n < image_count; ++n) {
        if (!images[n].header || images[n].unloading) continue;
        struct image *image = images + n;
        ++image->pins;
        snapshot[count] = image;
        headers[count] = (const void *)image->header;
        paths[count++] = image->path;
    }
    struct loader_transition transition;
    begin_transition(&transition);
    unsigned depth = release_loader();
    callback(count, headers, paths);
    restore_loader(depth);
    end_transition(&transition);
    for (unsigned n = 0; n < count; ++n) --snapshot[n]->pins;
    collect_images();
    unlock_loader();
}
static int section_name(const char field[16], const char *name) {
    for (unsigned n = 0; n < 16; ++n) {
        if (field[n] != name[n]) return 0;
        if (!field[n]) return 1;
    }
    return name[16] == 0;
}
unsigned char *getsectiondata(const struct mach_header_64 *header,
                                     const char *segment, const char *section,
                                     unsigned long *size) {
    LOADER_LOCKED();
    if (!size) return 0;
    *size = 0;
    if (!header || !segment || !section) return 0;
    for (unsigned i = 0; i < image_count; ++i) {
        const struct image *image = images + i;
        if (image->header != header) continue;
        for (unsigned n = 0; n < image->segment_count; ++n) {
            const struct segment_command_64 *s = image->segments[n];
            if (!section_name(s->segname, segment)) continue;
            const struct section_64 *sections = (const void *)(s + 1);
            for (unsigned j = 0; j < s->nsects; ++j) {
                const struct section_64 *found = sections + j;
                if (!section_name(found->segname, segment) || !section_name(found->sectname, section)) continue;
                require(found->addr >= s->vmaddr && found->size <= s->vmsize &&
                        found->addr - s->vmaddr <= s->vmsize - found->size,
                        "native section is outside its mapping");
                *size = found->size;
                return (void *)(found->addr + image->slide);
            }
        }
        return 0;
    }
    return 0;
}
/* ABI layouts from Apple's dyld_priv.h and dlfcn.h. The guest SDK installs the
 * original public headers; these internal names avoid requiring unrelated SDK
 * framework declarations in the freestanding loader. */
struct native_build_version { uint32_t platform, version; };
struct native_uuid_offset {
    unsigned char uuid[16];
    uint64_t offsetInImage;
    const struct mach_header *image;
};
struct native_dl_info {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
};

static const struct image *image_for_header(const struct mach_header *header) {
    if (!header) return 0;
    for (unsigned n = 0; n < image_count; ++n)
        if ((const void *)images[n].header == header) return images + n;
    return (const void *)linker_image.header == header ? &linker_image : 0;
}
static const struct segment_command_64 *segment_for_range(const struct image *image,
                                                         uintptr_t address, size_t size) {
    if (!size || size > UINTPTR_MAX - address) return 0;
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *segment = image->segments[n];
        uintptr_t start = segment->vmaddr + image->slide;
        if (segment->initprot && segment->vmsize <= UINTPTR_MAX - start &&
            address >= start && address - start < segment->vmsize &&
            size <= segment->vmsize - (address - start)) return segment;
    }
    return 0;
}
static const struct image *image_for_address(const void *address) {
    for (unsigned n = 0; n < image_count; ++n)
        if (segment_for_range(images + n, (uintptr_t)address, 1)) return images + n;
    return segment_for_range(&linker_image, (uintptr_t)address, 1) ? &linker_image : 0;
}
struct native_unwind_sections {
    const struct mach_header *header;
    const void *dwarf;
    uintptr_t dwarf_size;
    const void *compact;
    uintptr_t compact_size;
};
bool _dyld_find_unwind_sections(void *address, struct native_unwind_sections *result) {
    LOADER_LOCKED();
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    const struct image *image = image_for_address(address);
    if (!image || image->unloading) return false;
    result->header = (const void *)image->header;
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *segment = image->segments[n];
        if (!section_name(segment->segname, "__TEXT")) continue;
        const struct section_64 *sections = (const void *)(segment + 1);
        for (unsigned j = 0; j < segment->nsects; ++j) {
            const struct section_64 *section = sections + j;
            if (!section->size) continue;
            require(segment_for_range(image, section->addr + image->slide, section->size) != 0, "unwind section outside mapped image");
            if (section_name(section->sectname, "__eh_frame")) {
                result->dwarf = (const void *)(section->addr + image->slide); result->dwarf_size = section->size;
            } else if (section_name(section->sectname, "__unwind_info")) {
                result->compact = (const void *)(section->addr + image->slide); result->compact_size = section->size;
            }
        }
    }
    return true;
}
void _dyld_register_func_for_remove_image(remove_image_callback callback) {
    MUTATION_LOCKED(); LOADER_LOCKED();
    if (!callback) return;
    require(remove_image_callback_count < 64, "too many native image removal callbacks");
    retain_permanently((struct image *)image_for_address((void *)callback));
    remove_image_callbacks[remove_image_callback_count++] = callback;
}
const struct mach_header *_dyld_get_image_header(uint32_t index) {
    LOADER_LOCKED();
    for (unsigned n = 0; n < image_count; ++n)
        if (images[n].header && !images[n].unloading && !index--) return (const void *)images[n].header;
    return 0;
}
intptr_t _dyld_get_image_slide(const struct mach_header *header) {
    LOADER_LOCKED();
    const struct image *image = image_for_header(header);
    return image ? (intptr_t)image->slide : 0;
}
uint32_t dyld_get_active_platform(void) {
    LOADER_LOCKED(); return image_count ? images[0].platform : 0; }
uint64_t dyld_get_program_sdk_version_token(void) {
    LOADER_LOCKED();
    return image_count ? ((uint64_t)images[0].sdk << 32) | images[0].platform : 0;
}
uint32_t dyld_get_program_min_os_version(void) {
    LOADER_LOCKED();
    return image_count ? images[0].minimum_os : 0;
}
bool dyld_sdk_at_least(const struct mach_header *header, struct native_build_version version) {
    LOADER_LOCKED();
    const struct image *image = image_for_header(header);
    if (!image || !image->platform) return false;
    if (version.platform == UINT32_MAX) {
        unsigned count = sizeof(native_version_sets) / sizeof(native_version_sets[0]);
        unsigned n = 0;
        for (; n < count; ++n) if (native_version_sets[n].set >= version.version) break;
        if (n == count) return false;
        version.platform = PLATFORM_MACOS;
        version.version = native_version_sets[n].macos;
    }
    return image->platform == version.platform && image->sdk >= version.version;
}
bool dyld_program_sdk_at_least(struct native_build_version version) {
    LOADER_LOCKED();
    if (!image_count || !images[0].platform) return false;
    if (version.platform != UINT32_MAX)
        return dyld_sdk_at_least((const void *)images[0].header, version);
    uint32_t equivalent = 0;
    for (unsigned n = 0; n < sizeof(native_version_sets) / sizeof(native_version_sets[0]); ++n) {
        if (native_version_sets[n].macos > images[0].sdk) break;
        equivalent = native_version_sets[n].set;
    }
    return equivalent >= version.version;
}
static int library_name_matches(const char *path, const char *name) {
    const char *leaf = path;
    for (const char *p = path; *p; ++p) if (*p == '/') leaf = p + 1;
    if (same(leaf, name)) return 1;
    size_t leaf_size = length(leaf), name_size = length(name);
    if (name_size > SIZE_MAX - 9 || leaf_size < name_size + 9 ||
        leaf[0] != 'l' || leaf[1] != 'i' || leaf[2] != 'b' ||
        !same(leaf + leaf_size - 6, ".dylib")) return 0;
    for (size_t n = 0; n < name_size; ++n) if (leaf[n + 3] != name[n]) return 0;
    return leaf[name_size + 3] == '.';
}
int32_t NSVersionOfLinkTimeLibrary(const char *name) {
    LOADER_LOCKED();
    if (!name || !image_count) return -1;
    const unsigned char *cursor = (const void *)(images[0].header + 1);
    int32_t result = -1;
    for (unsigned n = 0; n < images[0].header->ncmds; ++n) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_LOAD_DYLIB || command->cmd == LC_LOAD_WEAK_DYLIB ||
            command->cmd == LC_REEXPORT_DYLIB || command->cmd == LC_LOAD_UPWARD_DYLIB) {
            const struct dylib_command *library = (const void *)command;
            if (library_name_matches(command_string(command, library->dylib.name.offset), name))
                result = (int32_t)library->dylib.current_version;
        }
        cursor += command->cmdsize;
    }
    return result;
}
bool _dyld_is_memory_immutable(const void *address, size_t size) {
    LOADER_LOCKED();
    const struct image *image = image_for_address(address);
    if (!image || !image->permanent) return false;
    const struct segment_command_64 *segment = segment_for_range(image, (uintptr_t)address, size);
    /* Only permanently retained images qualify.
     * A temporarily sealed DATA_CONST mapping can still permit later writes,
     * so maxprot, rather than its current read-only initprot, decides this. */
    return segment && !(segment->maxprot & 2);
}
static void query_security_policy(void) {
    if (security_policy.queried) return;
    security_policy.code_signing_known = csops(0, NATIVE_CS_OPS_STATUS,
        &security_policy.code_signing_flags, sizeof(security_policy.code_signing_flags)) == 0;
    security_policy.changed_credentials = issetugid() != 0;
    if (image_count) {
        const struct image *image = images;
        for (unsigned n = 0; n < image->segment_count; ++n) {
            const struct segment_command_64 *segment = image->segments[n];
            if (!section_name(segment->segname, "__RESTRICT")) continue;
            const struct section_64 *sections = (const void *)(segment + 1);
            for (unsigned j = 0; j < segment->nsects; ++j)
                if (section_name(sections[j].sectname, "__restrict"))
                    security_policy.restrict_section = 1;
        }
    }
    /* This loader has no AMFI policy provider and implements no DYLD_* path
     * overrides. Match Apple's failed-query rule: permissions stay disabled.
     * Kernel flags and credentials above are genuine observations, never
     * invented permission or AMFI-success results. */
    security_policy.environment_paths_allowed = 0;
    security_policy.queried = 1;
}
bool dyld_process_is_restricted(void) {
    LOADER_LOCKED();
    query_security_policy();
    if (!security_policy.code_signing_known || security_policy.changed_credentials ||
        security_policy.restrict_section ||
        (security_policy.code_signing_flags &
         (NATIVE_CS_RESTRICT | NATIVE_CS_REQUIRE_LV | NATIVE_CS_RUNTIME))) return true;
    return !security_policy.environment_paths_allowed;
}
bool _dyld_get_image_uuid(const struct mach_header *header, unsigned char uuid[16]) {
    LOADER_LOCKED();
    if (!uuid) return false;
    const struct image *image = image_for_header(header);
    if (image && image->uuid) { memcpy(uuid, image->uuid->uuid, 16); return true; }
    memset(uuid, 0, 16);
    return false;
}
const struct mach_header *dyld_image_header_containing_address(const void *address) {
    LOADER_LOCKED();
    const struct image *image = image_for_address(address);
    return image ? (const void *)image->header : 0;
}
void _dyld_images_for_addresses(unsigned count, const void *addresses[], struct native_uuid_offset infos[]) {
    LOADER_LOCKED();
    if (!addresses || !infos) return;
    for (unsigned n = 0; n < count; ++n) {
        memset(infos + n, 0, sizeof(*infos));
        const struct image *image = image_for_address(addresses[n]);
        if (!image) continue;
        infos[n].image = (const void *)image->header;
        infos[n].offsetInImage = (uintptr_t)addresses[n] - (uintptr_t)image->header;
        _dyld_get_image_uuid(infos[n].image, infos[n].uuid);
    }
}
void _dyld_stack_range(const void **bottom, const void **top) {
    /* dyld1378's ProtectedStack::getRange also returns null when hardware TPRO
     * is disabled. This loader uses the caller's stack, never a TPRO stack. */
    if (bottom) *bottom = 0;
    if (top) *top = 0;
}
const void *_dyld_get_shared_cache_range(size_t *size) {
    /* No shared cache is registered or mapped by this loader. */
    if (size) *size = 0;
    return 0;
}
bool _dyld_get_shared_cache_uuid(unsigned char uuid[16]) {
    (void)uuid; /* Apple's absent-cache result leaves the caller's buffer alone. */
    return false;
}
int dladdr(const void *address, struct native_dl_info *result) {
    LOADER_LOCKED();
    if (!result) return 0;
    memset(result, 0, sizeof(*result));
    const struct image *image = image_for_address(address);
    if (!image) return 0;
    result->dli_fname = image->path;
    result->dli_fbase = (void *)image->header;
    if (address == (const void *)image->header) {
        result->dli_sname = "__dso_handle";
        result->dli_saddr = (void *)image->header;
        return 1;
    }
    unsigned section_index = 0, target_index = 0;
    uint64_t target_start = 0;
    uint64_t target = (uintptr_t)address - image->slide;
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *segment = image->segments[n];
        const struct section_64 *sections = (const void *)(segment + 1);
        for (unsigned j = 0; j < segment->nsects; ++j) {
            ++section_index;
            if (target >= sections[j].addr && target - sections[j].addr < sections[j].size) {
                target_index = section_index;
                target_start = sections[j].addr;
            }
        }
    }
    if (!target_index || !image->symbols) return 1;
    const struct symtab_command *table = image->symbols;
    const struct nlist_64 *symbols = file_range(image, table->symoff, (uint64_t)table->nsyms * sizeof(*symbols));
    const char *strings = file_range(image, table->stroff, table->strsize);
    const struct nlist_64 *best = 0;
    /* Match dyld's preference for global symbols at equal addresses, then
     * inspect local symbols. Require both address and section to agree. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned n = 0; n < table->nsyms; ++n) {
            const struct nlist_64 *symbol = symbols + n;
            if ((symbol->n_type & N_STAB) || (symbol->n_type & N_TYPE) != N_SECT ||
                ((symbol->n_type & N_EXT) != 0) != (pass == 0) ||
                symbol->n_sect != target_index || symbol->n_value < target_start ||
                symbol->n_value > target || (best && best->n_value >= symbol->n_value)) continue;
            require(symbol->n_un.n_strx < table->strsize, "symbol name outside string table");
            const char *name = strings + symbol->n_un.n_strx;
            size_t limit = table->strsize - symbol->n_un.n_strx, size = 0;
            while (size < limit && name[size]) ++size;
            require(size < limit, "unterminated symbol name");
            best = symbol;
        }
    }
    if (best && best->n_value + image->slide != (uintptr_t)image->header) {
        result->dli_sname = strings + best->n_un.n_strx;
        if (*result->dli_sname == '_') ++result->dli_sname;
        result->dli_saddr = (void *)(best->n_value + image->slide);
    }
    return 1;
}
static uintptr_t loader_api(const char *name) {
#define API(function) if (same(name, "_" #function)) return (uintptr_t)function
    API(_dyld_register_for_bulk_image_loads); API(getsectiondata);
    API(_dyld_find_unwind_sections); API(_dyld_register_func_for_remove_image);
    API(_dyld_get_image_header); API(_dyld_get_image_slide);
    API(dyld_get_active_platform); API(dyld_get_program_sdk_version_token);
    API(dyld_get_program_min_os_version);
    API(dyld_program_sdk_at_least); API(dyld_sdk_at_least); API(NSVersionOfLinkTimeLibrary);
    API(_dyld_is_memory_immutable); API(dyld_process_is_restricted);
    API(_dyld_get_image_uuid); API(_dyld_images_for_addresses); API(_dyld_stack_range);
    API(_dyld_get_shared_cache_range); API(_dyld_get_shared_cache_uuid);
    API(dyld_image_header_containing_address); API(dladdr);
    API(dlopen); API(dlsym); API(dlclose); API(dlerror);
    API(_dyld_dlopen_atfork_prepare); API(_dyld_dlopen_atfork_parent); API(_dyld_dlopen_atfork_child);
    if (same(name, "_NXArgc")) return (uintptr_t)&NXArgc;
    if (same(name, "_NXArgv")) return (uintptr_t)&NXArgv;
    if (same(name, "_environ")) return (uintptr_t)&environ;
    if (same(name, "___progname")) return (uintptr_t)&__progname;
#undef API
    return 0;
}
static uintptr_t resolve(struct image *image, int ordinal, const char *name, unsigned flags) {
    uintptr_t address = 0;
    int found = 0;
    if (ordinal > 0) {
        require((unsigned)ordinal <= image->dependency_count, "invalid library ordinal");
        if (image->dependencies[ordinal - 1]) found = find_export(image->dependencies[ordinal - 1], name, 0, &address);
    } else if (ordinal == BIND_SPECIAL_DYLIB_SELF) found = find_export(image, name, 0, &address);
    else if (ordinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE) found = find_export(images, name, 0, &address);
    else if (ordinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP) found = find_coalesced(image, name, &address);
    else if (ordinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP) {
        for (unsigned n = 0; n < image_count && !found; ++n)
            if (!images[n].unloading && (images[n].global || in_transaction(images + n)))
                found = find_export(images + n, name, 0, &address);
        if (!found) { address = loader_api(name); found = address != 0; }
    } else fail("unsupported bind library ordinal", image->path);
    /* All lazy slots are eagerly bound; the resolver is never a successful fallback. */
    if (!found && same(name, "dyld_stub_binder")) { address = (uintptr_t)unresolved_lazy_symbol; found = 1; }
    if (!found && !(flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT)) fail("unresolved native import", name);
    if (address) remember_binding(image, address);
    return address;
}
static void bind(struct image *image, uint32_t start, uint32_t size, int lazy, int weak) {
    if (!size) return;
    struct stream stream = metadata(image, start, size);
    unsigned segment = 0, type = BIND_TYPE_POINTER, flags = 0;
    uint64_t offset = 0; int64_t addend = 0;
    int ordinal = weak ? BIND_SPECIAL_DYLIB_WEAK_LOOKUP : 0;
    const char *name = 0;
    while (stream.cursor < stream.end) {
        unsigned char byte = *stream.cursor++, opcode = byte & 0xf0, immediate = byte & 0xf;
        uint64_t count = 0, skip = 0;
        switch (opcode) {
        case BIND_OPCODE_DONE:
            if (!lazy) return;
            type = BIND_TYPE_POINTER; flags = segment = 0; offset = 0; addend = ordinal = 0; name = 0; continue;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: ordinal = immediate; continue;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
            uint64_t value = uleb(&stream); require(value <= MAX_DEPS, "library ordinal overflow");
            ordinal = (int)value; continue;
        }
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: ordinal = immediate ? (int)(signed char)(immediate | 0xf0) : 0; continue;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            flags = immediate; name = (const char *)stream.cursor;
            while (stream.cursor < stream.end && *stream.cursor) ++stream.cursor;
            require(stream.cursor < stream.end, "unterminated bind symbol"); ++stream.cursor; continue;
        case BIND_OPCODE_SET_TYPE_IMM: type = immediate; continue;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = sleb(&stream); continue;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: segment = immediate; offset = uleb(&stream); continue;
        /* Classic bind streams group symbols and can revisit an earlier slot
         * using a uint64 two's-complement delta. Apple's BindOpcodes.cpp uses
         * modular uint64 arithmetic, then validates each resulting target.
         * Keep fixup_slot's segment, size, protection and alignment checks. */
        case BIND_OPCODE_ADD_ADDR_ULEB: offset += uleb(&stream); continue;
        case BIND_OPCODE_DO_BIND: count = 1; break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: count = 1; skip = uleb(&stream); break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: count = 1; skip = immediate * sizeof(uintptr_t); break;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: count = uleb(&stream); skip = uleb(&stream); break;
        default: fail("unsupported native bind opcode", image->path);
        }
        require(type == BIND_TYPE_POINTER && name && count <= 1000000, "invalid pointer binding");
        uintptr_t address = resolve(image, ordinal, name, flags);
        for (uint64_t n = 0; n < count; ++n) {
            *fixup_slot(image, segment, offset) = address + (uintptr_t)addend;
            offset += sizeof(uintptr_t); offset += skip;
        }
    }
    require(lazy, "unterminated bind stream");
}

static void writable_fixups(struct image *image, int restore) {
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *s = image->segments[n];
        if (!s->vmsize || !(s->maxprot & 2) || (s->initprot & 4)) continue;
        int protection = restore ? s->initprot : s->initprot | 2;
        if (restore && (s->flags & SG_READ_ONLY)) protection &= ~2;
        require(__mprotect((void *)(s->vmaddr + image->slide), rounded(s->vmsize), protection) == 0,
                "cannot protect native data segment");
    }
}
static void coalesce_loaded_images(void) {
    for (unsigned n = 0; n < image_count; ++n) {
        struct image *image = images + n;
        if (!image->header || image->unloading || !image->info || !image->info->weak_bind_size) continue;
        writable_fixups(image, 0);
        bind(image, image->info->weak_bind_off, image->info->weak_bind_size, 0, 1);
        writable_fixups(image, 1);
    }
}
static void read_exact(int fd, void *data, size_t size, int64_t offset) {
    unsigned char *cursor = data;
    while (size) {
        long amount = __pread_nocancel(fd, cursor, size, offset);
        require(amount > 0 && (size_t)amount <= size, "cannot read Mach-O file");
        cursor += amount; size -= amount; offset += amount;
    }
}
static void map_library(struct image *image, int fd) {
    unsigned char header_bytes[sizeof(struct mach_header_64) + MAX_COMMANDS] __attribute__((aligned(16)));
    read_exact(fd, header_bytes, sizeof(struct mach_header_64), 0);
    struct mach_header_64 *header = (void *)header_bytes;
    require(header->sizeofcmds <= MAX_COMMANDS, "library command area too large");
    read_exact(fd, header + 1, header->sizeofcmds, sizeof(*header));
    uint32_t kind = header->filetype;
    require(kind == MH_DYLIB || kind == MH_BUNDLE, "native module must be a dylib or bundle");
    parse_image(image, header, kind);
    uint64_t minimum = UINT64_MAX, maximum = 0;
    require(image->signature && image->signature->datasize,
            "library lacks a native code signature");
    uint64_t size = (uint64_t)image->signature->dataoff + image->signature->datasize;
    unsigned char tail;
    read_exact(fd, &tail, 1, size - 1);
    require(__pread_nocancel(fd, &tail, 1, size) == 0, "signature must end the native library file");
    struct { int64_t start; void *blob; size_t size; } signature = {
        0, (void *)(uintptr_t)image->signature->dataoff, image->signature->datasize};
    if (__fcntl_nocancel(fd, 61, &signature)) fail("CodeDirectory registration rejected", image->path);
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *s = image->segments[n];
        require(s->fileoff <= size && s->filesize <= size - s->fileoff, "segment outside library file");
        if (!s->vmsize || (!s->initprot && !s->maxprot)) continue;
        if (s->vmaddr < minimum) minimum = s->vmaddr;
        if (s->vmaddr + s->vmsize > maximum) maximum = s->vmaddr + s->vmsize;
    }
    require(minimum != UINT64_MAX && maximum > minimum && maximum - minimum <= 1024UL * 1024 * 1024,
            "invalid native library extent");
    void *reservation = __mmap(0, rounded(maximum - minimum), 0, 0x1002, -1, 0);
    if (reservation == (void *)-1) fail("cannot reserve native library", image->path);
    image->reservation = reservation;
    image->reservation_size = rounded(maximum - minimum);
    uintptr_t slide = (uintptr_t)reservation - minimum;
    uintptr_t mapped_header = image->preferred + slide;
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *s = image->segments[n];
        if (!s->vmsize || (!s->initprot && !s->maxprot)) continue;
        uintptr_t address = s->vmaddr + slide;
        uint64_t file_bytes = rounded(s->filesize), memory_bytes = rounded(s->vmsize);
        if (file_bytes) {
            void *mapped = __mmap((void *)address, file_bytes, s->initprot, 0x12, fd, s->fileoff);
            if (mapped != (void *)address) fail("cannot map signed native segment", image->path);
        }
        if (memory_bytes > file_bytes) {
            void *mapped = __mmap((void *)(address + file_bytes), memory_bytes - file_bytes,
                                  s->initprot, 0x1012, -1, 0);
            if (mapped != (void *)(address + file_bytes)) fail("cannot map native zero-fill", image->path);
        }
        if ((s->initprot & 2) && s->vmsize > s->filesize && s->filesize < file_bytes) {
            require(!(s->initprot & 4), "executable zero-fill tail");
            memset((void *)(address + s->filesize), 0, file_bytes - s->filesize);
        }
    }
    /* Replace all pointers into the temporary command buffer with mapped metadata. */
    image->segment_count = 0; image->info = 0; image->symbols = 0; image->signature = 0;
    image->uuid = 0; image->platform = 0; image->minimum_os = 0; image->sdk = 0;
    parse_image(image, (const void *)mapped_header, kind);
}
static int prefix(const char *text, const char *start) {
    while (*start) if (*text++ != *start++) return 0;
    return 1;
}
static void expand_path(char *out, const char *path, struct image *owner) {
    const char *tail = path;
    const char *base = 0;
    if (same(path, "@loader_path")) { base = owner->path; tail += 12; }
    else if (same(path, "@executable_path")) { base = images[0].path; tail += 16; }
    else if (prefix(path, "@loader_path/")) { base = owner->path; tail += 13; }
    else if (prefix(path, "@executable_path/")) { base = images[0].path; tail += 17; }
    if (base) {
        size_t end = length(base);
        while (end && base[end - 1] != '/') --end;
        require(end + length(tail) < MAX_PATH, "expanded library path too long");
        memcpy(out, base, end); copy_path(out + end, tail);
    } else {
        require(path[0] != '@', "unsupported native library path token");
        copy_path(out, path);
    }
}
static struct image *registered_path(const char *path) {
    for (unsigned n = 0; n < image_count; ++n)
        if (images[n].header && same(images[n].path, path)) {
            require(!images[n].unloading, "native library is being unloaded");
            return images + n;
        }
    return 0;
}
static int open_library_path(char path[MAX_PATH], const char *name, struct image *owner,
                             struct image **existing) {
    if (!prefix(name, "@rpath/")) {
        expand_path(path, name, owner);
        if ((*existing = registered_path(path))) return -1;
        return __open_nocancel(path, 0, 0);
    }
    struct image *cursor = owner;
    unsigned visited = 0;
    while (cursor && visited++ <= MAX_IMAGES) {
        require(cursor->header != 0, "unloaded native rpath owner");
        const unsigned char *commands = (const void *)(cursor->header + 1);
        for (unsigned n = 0; n < cursor->header->ncmds; ++n) {
            const struct load_command *command = (const void *)commands;
            if (command->cmd == LC_RPATH) {
                char directory[MAX_PATH];
                expand_path(directory, command_string(command, ((const uint32_t *)command)[2]), cursor);
                size_t size = length(directory);
                require(size + length(name + 7) + 2 <= MAX_PATH, "expanded rpath too long");
                memcpy(path, directory, size);
                if (!size || path[size - 1] != '/') path[size++] = '/';
                copy_path(path + size, name + 7);
                if ((*existing = registered_path(path))) return -1;
                int fd = __open_nocancel(path, 0, 0);
                if (fd >= 0) return fd;
            }
            commands += command->cmdsize;
        }
        if (cursor == images) break;
        cursor = cursor->load_parent ? cursor->load_parent : images;
    }
    return -1;
}
static void load_dependencies(struct image *image);
static struct image *load_library(const char *name, int weak, struct image *owner) {
    char path[MAX_PATH];
    struct image *existing = 0;
    int fd = open_library_path(path, name, owner, &existing);
    if (existing) return existing;
    if (fd < 0) { if (weak) return 0; fail("cannot open native library", name); }
    /* F_GETPATH resolves loader/executable/rpath aliases to the actual vnode
     * pathname, without importing libc's allocator-dependent realpath. */
    char canonical[MAX_PATH];
    if (__fcntl_nocancel(fd, NATIVE_F_GETPATH, canonical)) {
        __close_nocancel(fd);
        fail("cannot obtain native library path", path);
    }
    for (unsigned n = 0; n < image_count; ++n) {
        if (images[n].header && same(images[n].path, canonical)) {
            __close_nocancel(fd);
            require(!images[n].unloading, "native library is being unloaded");
            return images + n;
        }
    }
    if (active_transaction && active_transaction->no_load) {
        __close_nocancel(fd);
        fail("native library is not loaded", canonical);
    }
    unsigned index = 1;
    while (index < image_count && images[index].header) ++index;
    if (index == MAX_IMAGES) { __close_nocancel(fd); fail("too many native libraries", name); }
    if (index == image_count) ++image_count;
    struct image *image = images + index;
    memset(image, 0, sizeof(*image));
    require(load_sequence < UINT64_MAX, "native image load-order overflow");
    image->load_order = ++load_sequence;
    image->descriptor = fd;
    image->load_parent = owner;
    image->loading_thread = active_transaction ? native_thread_identity() : 0;
    copy_path(image->path, canonical);
    if (active_transaction) active_transaction->created[active_transaction->count++] = image;
    else image->permanent = image->global = 1;
    map_library(image, fd);
    require(__close_nocancel(fd) == 0, "cannot close native library");
    image->descriptor = -1;
    load_dependencies(image);
    return image;
}
static void load_dependencies(struct image *image) {
    const unsigned char *commands = (const void *)(image->header + 1);
    uint32_t cursor = 0;
    for (unsigned n = 0; n < image->header->ncmds; ++n) {
        const struct load_command *command = (const void *)(commands + cursor);
        if (command->cmd == LC_LOAD_DYLIB || command->cmd == LC_LOAD_WEAK_DYLIB ||
            command->cmd == LC_REEXPORT_DYLIB || command->cmd == LC_LOAD_UPWARD_DYLIB) {
            require(image->dependency_count < MAX_DEPS, "too many native dependencies");
            const struct dylib_command *dylib = (const void *)command;
            unsigned index = image->dependency_count++;
            image->reexports[index] = command->cmd == LC_REEXPORT_DYLIB;
            image->dependencies[index] = load_library(command_string(command, dylib->dylib.name.offset),
                command->cmd == LC_LOAD_WEAK_DYLIB, image);
        }
        cursor += command->cmdsize;
    }
}
static void initialize(struct image *image, int argc, char **argv, char **environment, char **apple) {
    lock_loader();
    uint64_t self = native_thread_identity();
    require(mutation_lock_owner == self, "native initialization requires mutation lock");
    while (image->loading_thread && image->loading_thread != self) {
        wait_loader();
    }
    if (image->loading_thread == self) { unlock_loader(); return; }
    while (image->initialized == 1 && image->initializing_thread != self) {
        wait_loader();
    }
    if (image->initialized) { unlock_loader(); return; }
    image->initialized = 1;
    image->initializing_thread = self;
    ++image->pins;
    struct loader_transition transition;
    begin_transition(&transition);
    unsigned depth = release_loader();
    for (unsigned n = 0; n < image->dependency_count; ++n)
        if (image->dependencies[n]) initialize(image->dependencies[n], argc, argv, environment, apple);
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *s = image->segments[n];
        const struct section_64 *sections = (const void *)(s + 1);
        for (unsigned j = 0; j < s->nsects; ++j) if ((sections[j].flags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS) {
            const struct section_64 *section = sections + j;
            require(section->addr >= s->vmaddr && section->size <= s->vmsize &&
                    section->addr - s->vmaddr <= s->vmsize - section->size && !(section->size & 7),
                    "invalid native constructor section");
            void (**functions)(int, char **, char **, char **, const struct native_program_vars *) =
                (void *)(section->addr + image->slide);
            for (uint64_t k = 0; k < section->size / sizeof(*functions); ++k)
                functions[k](argc, argv, environment, apple, &program_vars);
        }
    }
    restore_loader(depth);
    image->initialized = 2;
    image->initialization_order = ++initialization_sequence;
    image->initializing_thread = 0;
    --image->pins;
    end_transition(&transition);
    unlock_loader();
}
#include "dynamic.c"
_Noreturn void native_dyld_entry(const struct mach_header_64 *header, uintptr_t *stack) {
    /* These local addresses are PC-relative and need no pointer rebasing. */
    bootstrap_tsd[1] = &bootstrap_errno;
    _thread_set_tsd_base(bootstrap_tsd);
    bootstrap_tsd[3] = (void *)(uintptr_t)thread_self_trap();
    /* Bootstrap our own pointer rebases before accessing libsyscall's errno GOT. */
    parse_image(&linker_image, &_mh_dylinker_header, MH_DYLINKER);
    rebase(&linker_image);
    copy_path(linker_image.path, "/usr/lib/dyld");
    linker_image.permanent = 1;
    image_count = 1;
    parse_image(images, header, MH_EXECUTE);
    images[0].permanent = images[0].global = 1;
    images[0].descriptor = -1;
    require(header->flags & MH_DYLDLINK, "main image is not dynamically linked");
    require(images->platform == PLATFORM_MACOS, "main image lacks native macOS version metadata");
    query_security_policy();
    int argc = (int)stack[1]; char **argv = (void *)(stack + 2);
    require(argc > 0 && argc < 4096, "invalid native argc");
    char **environment = argv + argc + 1, **apple = environment;
    while (*apple) ++apple;
    ++apple;
    const char *executable = argv[0];
    for (char **argument = apple; *argument; ++argument) {
        const char *prefix = "executable_path=";
        size_t n = 0;
        while (prefix[n] && (*argument)[n] == prefix[n]) ++n;
        if (!prefix[n]) { executable = *argument + n; break; }
    }
    copy_path(images->path, executable);
    NXArgc = argc; NXArgv = argv;
    environ = environment; process_apple = apple;
    __progname = executable;
    for (const char *p = executable; *p; ++p) if (*p == '/') __progname = p + 1;
    program_vars = (struct native_program_vars){header, &NXArgc, &NXArgv,
                                               &environ, &__progname};
    load_dependencies(images);
    for (unsigned n = 0; n < image_count; ++n) {
        struct image *image = images + n;
        writable_fixups(image, 0); rebase(image);
        if (image->info) {
            bind(image, image->info->bind_off, image->info->bind_size, 0, 0);
            bind(image, image->info->lazy_bind_off, image->info->lazy_bind_size, 1, 0);
            bind(image, image->info->weak_bind_off, image->info->weak_bind_size, 0, 1);
        }
        writable_fixups(image, 1);
    }
    for (unsigned n = 0; n < image_count; ++n) validate_module(images + n);
    lock_mutation();
    initialize(images, argc, argv, environment, apple);
    unlock_mutation();
    require(images->entry_offset != UINT64_MAX, "main image lacks LC_MAIN");
    for (unsigned n = 0; n < images->segment_count; ++n) {
        const struct segment_command_64 *s = images->segments[n];
        if ((s->initprot & 4) && !(s->initprot & 2) && images->entry_offset >= s->fileoff &&
            images->entry_offset - s->fileoff < s->filesize) {
            int (*main)(int, char **, char **, char **) =
                (void *)(s->vmaddr + images->slide + images->entry_offset - s->fileoff);
            int status = main(argc, argv, environment, apple);
            lock_loader();
            void (*runtime_exit)(int) = (void *)runtime_symbol("_exit");
            unlock_loader();
            if (runtime_exit) runtime_exit(status);
            __exit(status);
        }
    }
    fail("main entry is not executable", images->path);
}
