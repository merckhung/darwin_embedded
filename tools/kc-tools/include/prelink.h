#pragma once
#include "linker.h"
#include <stdint.h>
#include <stddef.h>

int prelink_assemble(const uint8_t *kernel_data, size_t kernel_size,
                     LinkedKext *kexts, size_t num_kexts,
                     uint8_t **out_data, size_t *out_size);

// Assemble an MH_FILESET kernel collection (KC). See src/fileset.c.
// kc_base != 0 emits a collection mach_header of its own at that address,
// separate from the kernel's. 0 keeps the legacy shared-header layout.
int fileset_assemble(const uint8_t *kernel_data, size_t kernel_size,
                     LinkedKext *kexts, size_t num_kexts, uint64_t kc_base,
                     uint8_t **out_data, size_t *out_size);
