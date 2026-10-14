// SPDX-License-Identifier: BSD-2-Clause
#include "image.h"

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t wide(const uint8_t *p) { return word(p) | (uint64_t)word(p + 4) << 32; }

int pc_image_parse(const void *data, size_t size, struct pc_image *out) {
    const uint8_t *file = data;
    if (!file || !out || size < 32 || word(file) != 0xfeedfacf ||
        word(file + 4) != 0x01000007 || word(file + 12) != 12 ||
        !(word(file + 24) & 0x80000000u)) return 0;
    *out = (struct pc_image){.file = file, .file_size = size, .base = UINT64_MAX};
    uint32_t count = word(file + 16), bytes = word(file + 20);
    if (bytes > size - 32 || count > bytes / 8) return 0;
    size_t cursor = 32, end = 32 + (size_t)bytes;
    uint64_t high = 0, entry = 0, header = 0;
    unsigned int entries = 0, headers = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (end - cursor < 8) return 0;
        const uint8_t *p = file + cursor;
        uint32_t kind = word(p), length = word(p + 4);
        if (length < 8 || length > end - cursor || (length & 7)) return 0;
        if (kind == 0x19) {
            if (length < sizeof(struct pc_segment) || out->segment_count == 64) return 0;
            const struct pc_segment *s = (const struct pc_segment *)p;
            if ((s->address & 0xfff) ||
                s->address < 0xffffff8000000000ULL ||
                s->memory_size > UINT64_MAX - s->address || s->file_size > s->memory_size ||
                s->file_offset > size || s->file_size > size - s->file_offset ||
                s->sections > (length - sizeof(*s)) / 80) return 0;
            if (s->memory_size) {
                for (unsigned int j = 0; j < out->segment_count; ++j) {
                    const struct pc_segment *other = out->segments[j];
                    // Collection aggregate extents may include another
                    // segment's zero-fill tail. File-backed bytes must remain
                    // disjoint; the entire staging image is zeroed once.
                    if (s->file_size && other->file_size &&
                        s->address < other->address + other->file_size &&
                        other->address < s->address + s->file_size) return 0;
                }
                out->segments[out->segment_count++] = s;
                if (s->address < out->base) out->base = s->address;
                if (s->address + s->memory_size > high) high = s->address + s->memory_size;
                if (!s->file_offset && s->file_size >= end) { header = s->address; ++headers; }
            }
        } else if (kind == 5) { // LC_UNIXTHREAD, x86_THREAD_STATE64
            size_t at = 8;
            while (at < length) {
                if (length - at < 8) return 0;
                uint32_t flavor = word(p + at), words = word(p + at + 4);
                at += 8;
                if (words > (length - at) / 4) return 0;
                if (flavor == 4) {
                    if (words != 42) return 0;
                    entry = wide(p + at + 16 * 8);
                    ++entries;
                }
                at += (size_t)words * 4;
            }
        }
        cursor += length;
    }
    if (cursor != end || !out->segment_count || entries != 1 || headers != 1 ||
        high - out->base > 0x2f00000) return 0;
    int executable_entry = 0;
    for (unsigned int i = 0; i < out->segment_count; ++i) {
        const struct pc_segment *s = out->segments[i];
        // XNU's __HIB holds the bootstrap tables and native 64-bit entry.
        // It is read/write until the kernel installs its final protections.
        int bootstrap = s->name[0] == '_' && s->name[1] == '_' && s->name[2] == 'H' &&
                        s->name[3] == 'I' && s->name[4] == 'B' && s->name[5] == 0;
        if (((s->initial_protection & 4) || bootstrap) && entry >= s->address &&
            s->file_size >= 16 && entry - s->address <= s->file_size - 16) {
            // Entry bytes must be file-backed, bounded and explicitly opt in
            // to this ABI. LC_UNIXTHREAD alone does not specify execution mode.
            static const uint8_t signature[16] = PC_ENTRY_SIGNATURE;
            const uint8_t *candidate = file + s->file_offset + (entry - s->address);
            for (unsigned int n = 0; n < sizeof(signature); ++n)
                if (candidate[n] != signature[n]) return 0;
            executable_entry = 1;
        }
    }
    if (!executable_entry) return 0;
    out->size = high - out->base;
    out->entry = entry - out->base;
    out->header_address = header - out->base;
    return 1;
}

void pc_image_load(const struct pc_image *image, void *destination) {
    uint8_t *base = destination;
    for (uint64_t i = 0; i < image->size; ++i) base[i] = 0;
    for (unsigned int i = 0; i < image->segment_count; ++i) {
        const struct pc_segment *s = image->segments[i];
        for (uint64_t n = 0; n < s->file_size; ++n)
            base[s->address - image->base + n] = image->file[s->file_offset + n];
    }
}
