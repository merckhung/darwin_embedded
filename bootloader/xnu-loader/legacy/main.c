#include "efi_emulation.h"
#include "legacy.h"
#include "serial.h"

static EfiEmuBootInfo boot_info;

void legacy_firmware_main(LegacyE820Entry *map, UINT32 count,
                          UINT32 boot_drive, LegacyFramebuffer *framebuffer,
                          EfiEmuBiosRead bios_read) {
  /* Program COM1 first; the BIOS may have left any baud rate set. */
  serial_reinit();
  efiemu_exceptions_install();
  efiemu_debug_string("legacy: BIOS stage2 entry\n");

  for (UINT32 i = 0; i < count && i < EFIEMU_MAX_MEMORY_RANGES; ++i) {
    boot_info.memory[i].base = map[i].base;
    boot_info.memory[i].length = map[i].length;
    boot_info.memory[i].type = map[i].type;
    boot_info.memory_count = i + 1;
  }
  if (framebuffer && framebuffer->valid) {
    EfiEmuFramebuffer *fb = &boot_info.framebuffer;
    fb->base = framebuffer->base;
    fb->width = framebuffer->width;
    fb->height = framebuffer->height;
    fb->pixels_per_scanline = framebuffer->pixels_per_scanline;
    fb->bits_per_pixel = framebuffer->bits_per_pixel;
    fb->red_position = framebuffer->red_position;
    fb->blue_position = framebuffer->blue_position;
    fb->valid = 1;
  }
  /* Bootstrap page tables and the BIOS stages live below the payload. */
  boot_info.protocol_data_base = 0x1000;
  boot_info.protocol_data_size = 0x20000 - 0x1000;
  /* Hard-disk numbers only: 0x00-0x7f are floppies, which carry no GPT/MBR. */
  if (bios_read && boot_drive >= 0x80)
    efiemu_bios_disk_set(boot_drive, bios_read);
  /* No modules: efi-emulation boots from the FAT32 disk. */
  efiemu_main(&boot_info);
}
