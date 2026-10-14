// SPDX-License-Identifier: BSD-2-Clause
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)
static union { uint64_t align; uint8_t bytes[8192]; } input;
static const uint64_t vm = 0xffffff8000100000ULL;
static void put32(size_t at, uint32_t n) { memcpy(input.bytes + at, &n, 4); }
static void put64(size_t at, uint64_t n) { memcpy(input.bytes + at, &n, 8); }
static void fixture(void) {
    memset(input.bytes, 0, sizeof(input.bytes));
    put32(0, 0xfeedfacf); put32(4, 0x01000007); put32(12, 12);
    put32(16, 3); put32(20, 72 * 2 + 184); put32(24, 0x80000000);
    struct pc_segment first = {.command=0x19, .command_size=72, .address=vm,
        .memory_size=4096, .file_size=4096, .initial_protection=5};
    struct pc_segment second = {.command=0x19, .command_size=72, .address=vm+8192,
        .memory_size=4096, .file_offset=4096, .file_size=16, .initial_protection=3};
    memcpy(input.bytes+32, &first, sizeof(first)); memcpy(input.bytes+104, &second, sizeof(second));
    put32(176, 5); put32(180, 184); put32(184, 4); put32(188, 42); put64(320, vm+512);
    static const uint8_t signature[16] = PC_ENTRY_SIGNATURE;
    memcpy(input.bytes+512, signature, sizeof(signature));
    memset(input.bytes+4096, 0x5a, 16);
}
int main(void) {
    struct pc_image image;
    fixture(); CHECK(pc_image_parse(input.bytes, sizeof(input.bytes), &image));
    CHECK(image.base == vm && image.size == 12288 && image.entry == 512 && image.header_address == 0);
    uint8_t destination[12288]; memset(destination, 0xff, sizeof(destination));
    pc_image_load(&image, destination);
    CHECK(!memcmp(destination, input.bytes, 4096));
    CHECK(!memcmp(destination+8192, input.bytes+4096, 16));
    for (size_t i = 4096; i < 8192; ++i) CHECK(destination[i] == 0);
    for (size_t i = 8208; i < sizeof(destination); ++i) CHECK(destination[i] == 0);
    // Real collections contain aggregate zero-fill extents over later segments.
    put64(64, 12288); CHECK(pc_image_parse(input.bytes, sizeof(input.bytes), &image));
    pc_image_load(&image, destination); CHECK(destination[8192] == 0x5a); fixture();
    // The native entry is file-backed even when __HIB is read/write.
    memcpy(input.bytes + 40, "__HIB", 6); put32(92, 3);
    CHECK(pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    // A legacy entry, different contract version or truncated tag cannot boot.
    input.bytes[512] = 0x90; CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    input.bytes[522] = 2; CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    input.bytes[527] = 1; CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(320, vm+4096-15); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    CHECK(!pc_image_parse(input.bytes, 31, &image));
    CHECK(!pc_image_parse(input.bytes, 359, &image));
    put32(4, 0x0100000c); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put32(12, 2); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put32(36, 7); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    // Link-edit data may end inside a page; segment addresses must be aligned.
    put64(136, 4095); CHECK(pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(128, vm+8193); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(144, 8193); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(152, 8192); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(136, UINT64_MAX); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(128, vm); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(128, vm+0x4000000); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put64(320, vm+8192); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put32(188, UINT32_MAX); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image)); fixture();
    put32(16, 4); CHECK(!pc_image_parse(input.bytes, sizeof(input.bytes), &image));
    puts("PC collection parser: validated loading, BSS/gaps, wrong architectures and malformed ranges");
    return 0;
}
