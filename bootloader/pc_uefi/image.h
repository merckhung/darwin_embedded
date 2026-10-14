// SPDX-License-Identifier: BSD-2-Clause
#ifndef PC_UEFI_IMAGE_H
#define PC_UEFI_IMAGE_H
#include <stddef.h>
#include <stdint.h>

// The first instruction skips a versioned entry tag in XNU's __start.
// Checking the complete tag prevents calling a legacy entry in long mode.
#define PC_ENTRY_SIGNATURE {0xeb, 0x0e, 'D', 'E', '6', '4', 'B', 'O', 'O', 'T', 1, 0, 0, 0, 0, 0}

// Subset of the public Mach-O ABI consumed by the embedded PC boot path.
struct pc_segment {
    uint32_t command, command_size;
    char name[16];
    uint64_t address, memory_size, file_offset, file_size;
    uint32_t maximum_protection, initial_protection, sections, flags;
};
struct pc_image {
    const uint8_t *file;
    size_t file_size;
    uint64_t base, size, entry, header_address;
    const struct pc_segment *segments[64];
    unsigned int segment_count;
};

// Only little-endian x86_64 MH_FILESET collections are accepted. Every range
// is validated before loading; no FAT images, slide relocations or netboot.
int pc_image_parse(const void *file, size_t size, struct pc_image *out);
void pc_image_load(const struct pc_image *image, void *destination);
#endif
