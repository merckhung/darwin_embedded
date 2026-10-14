/* Diagnostic Mach-O launcher: no libraries, bindings or runtime fixups.
 * XNU maps and checks the executable and this MH_DYLINKER before entry.
 * This supports only the position-independent programs in this directory.
 */
#include <stdint.h>
#include <stddef.h>

struct header { uint32_t magic, cpu, subtype, type, count, size, flags, reserved; };
struct command { uint32_t type, size; };
struct segment {
    struct command command;
    char name[16];
    uint64_t address, memory_size, offset, file_size;
    uint32_t maximum, protection, sections, flags;
};
struct main_command { struct command command; uint64_t offset, stack_size; };

uintptr_t diagnostic_entry(const struct header *header) {
    if (header->magic != 0xfeedfacf || header->type != 2 ||
        (header->flags & 0x200004) != 0x200004 ||
        !header->count || header->count > 64 || header->size > 16384)
        return 0;
    const unsigned char *begin = (const unsigned char *)(header + 1);
    size_t cursor = 0;
    uint64_t entry_offset = UINT64_MAX, preferred_header = UINT64_MAX;
    for (uint32_t i = 0; i < header->count; ++i) {
        if (cursor > header->size || header->size - cursor < sizeof(struct command)) return 0;
        const struct command *command = (const void *)(begin + cursor);
        if (command->size < 8 || (command->size & 7) || command->size > header->size - cursor) return 0;
        switch (command->type) {
        case 0x19: { /* LC_SEGMENT_64 */
            if (command->size < sizeof(struct segment)) return 0;
            const struct segment *segment = (const void *)command;
            if (!segment->offset && segment->file_size) {
                if (preferred_header != UINT64_MAX || segment->file_size < sizeof(*header) + header->size) return 0;
                preferred_header = segment->address;
            }
            break;
        }
        case 0x80000028: /* LC_MAIN */
            if (command->size != sizeof(struct main_command) || entry_offset != UINT64_MAX) return 0;
            entry_offset = ((const struct main_command *)command)->offset;
            break;
        case 0x22: case 0x80000022: { /* LC_DYLD_INFO[_ONLY]: no rebases or bindings */
            if (command->size != 48) return 0;
            const uint32_t *fields = (const void *)command;
            if (fields[3] || fields[5] || fields[7] || fields[9]) return 0;
            break;
        }
        /* Metadata and the kernel-selected interpreter; no dependent libraries. */
        case 2: case 0xb: case 0xe: case 0x1b: case 0x1d:
        case 0x24: case 0x26: case 0x29: case 0x32:
            break;
        default: return 0;
        }
        cursor += command->size;
    }
    if (cursor != header->size || preferred_header == UINT64_MAX || entry_offset == UINT64_MAX) return 0;
    cursor = 0;
    for (uint32_t i = 0; i < header->count; ++i) {
        const struct command *command = (const void *)(begin + cursor);
        if (command->type == 0x19) {
            const struct segment *segment = (const void *)command;
            if ((segment->protection & 4) && !(segment->protection & 2) &&
                entry_offset >= segment->offset && entry_offset - segment->offset < segment->file_size) {
                uint64_t delta = segment->address - preferred_header + entry_offset - segment->offset;
                if (delta > UINTPTR_MAX - (uintptr_t)header) return 0;
                return (uintptr_t)header + delta;
            }
        }
        cursor += command->size;
    }
    return 0;
}
