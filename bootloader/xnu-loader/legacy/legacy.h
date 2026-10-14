#ifndef XNU_LEGACY_H
#define XNU_LEGACY_H

#include <efi.h>
#include "efi_emulation.h"

typedef struct __attribute__((packed)) {
  UINT64 base;
  UINT64 length;
  UINT32 type;
  UINT32 attributes;
} LegacyE820Entry;

typedef struct __attribute__((packed)) {
  UINT64 base;
  UINT32 width;
  UINT32 height;
  UINT32 pixels_per_scanline;
  UINT8 bits_per_pixel;
  UINT8 red_position;
  UINT8 blue_position;
  UINT8 valid;
} LegacyFramebuffer;

/* Called by stage2 in long mode; hands the BIOS data to efi-emulation. The
 * BIOS read thunk lets the loader reach disks only the BIOS can see (USB). */
void legacy_firmware_main(LegacyE820Entry *map, UINT32 count,
                          UINT32 boot_drive, LegacyFramebuffer *framebuffer,
                          EfiEmuBiosRead bios_read);

#endif
