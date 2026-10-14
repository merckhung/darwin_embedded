// SPDX-License-Identifier: BSD-2-Clause
#include "macho_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
int main(void) {
    struct {
        struct mach_header_64 header;
        struct segment_command_64 segment;
    } file = {0};
    MachoView view;
    file.header.magic = MH_MAGIC_64;
    file.header.cputype = CPU_TYPE_X86_64;
    CHECK(!macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.header.cputype = CPU_TYPE_ARM64;
    CHECK(!macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.header.cputype = 24 | 0x01000000; // Unsupported RISC-V.
    CHECK(macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.header.cputype = 12; // Unsupported 32-bit ARM.
    CHECK(macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.header.cputype = CPU_TYPE_X86_64;
    file.header.magic = MH_MAGIC;
    CHECK(macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.header.magic = MH_MAGIC_64;
    CHECK(macho_view((const uint8_t *)&file, sizeof(file.header) - 1, &view));
    file.header.ncmds = 1; file.header.sizeofcmds = sizeof(file.segment);
    file.segment.cmd = LC_SEGMENT_64; file.segment.cmdsize = sizeof(file.segment);
    CHECK(!macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.segment.nsects = 1;
    CHECK(macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.segment.nsects = 0; file.segment.cmdsize = 0;
    CHECK(macho_view((const uint8_t *)&file, sizeof(file), &view));
    file.segment.cmdsize = sizeof(file.segment) + 8;
    CHECK(macho_view((const uint8_t *)&file, sizeof(file), &view));
    puts("Collection view: supported CPUs and malformed command rejection passed");
}
