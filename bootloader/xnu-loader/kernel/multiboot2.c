#include "efi_emulation.h"
#include "serial.h"

#define MB2_BOOTLOADER_MAGIC 0x36d76289U

#define MB2_TAG_END 0
#define MB2_TAG_CMDLINE 1
#define MB2_TAG_MODULE 3
#define MB2_TAG_MMAP 6
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_TAG_EFI64_SYSTEM_TABLE 12
#define MB2_TAG_ACPI_OLD 14
#define MB2_TAG_ACPI_NEW 15

typedef struct __attribute__((packed)) {
  UINT32 type;
  UINT32 size;
} Mb2Tag;

typedef struct __attribute__((packed)) {
  UINT64 base;
  UINT64 length;
  UINT32 type;
  UINT32 reserved;
} Mb2MmapEntry;

static EfiEmuBootInfo boot_info;

static void parse_framebuffer(CONST UINT8 *tag) {
  UINT64 base = 0;
  UINT32 pitch = 0, width = 0, height = 0;
  for (UINTN i = 0; i < 8; ++i)
    base |= (UINT64)tag[8 + i] << (i * 8);
  for (UINTN i = 0; i < 4; ++i) {
    pitch |= (UINT32)tag[16 + i] << (i * 8);
    width |= (UINT32)tag[20 + i] << (i * 8);
    height |= (UINT32)tag[24 + i] << (i * 8);
  }
  UINT8 bpp = tag[28];
  UINT8 type = tag[29];
  /* Only direct RGB framebuffers can become a GOP; text mode stays serial. */
  if (type != 1 || bpp != 32 || base == 0 || base + (UINT64)pitch * height > 0x100000000ULL)
    return;
  EfiEmuFramebuffer *fb = &boot_info.framebuffer;
  fb->base = base;
  fb->width = width;
  fb->height = height;
  fb->pixels_per_scanline = pitch / 4;
  fb->bits_per_pixel = bpp;
  fb->red_position = tag[32];
  fb->blue_position = tag[36];
  fb->valid = 1;
}

void kernel_multiboot2_main(UINT64 magic, UINT64 mbi) {
  efiemu_debug_string("xnu-loader kernel: multiboot2 entry\n");
  if (magic != MB2_BOOTLOADER_MAGIC) {
    efiemu_debug_string("xnu-loader kernel: bad multiboot2 magic\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }

  CONST UINT8 *info = (CONST UINT8 *)(UINTN)mbi;
  UINT32 total = *(UINT32 *)(UINTN)mbi;
  boot_info.protocol_data_base = mbi;
  boot_info.protocol_data_size = total;

  for (UINT32 offset = 8; offset + sizeof(Mb2Tag) <= total;) {
    CONST Mb2Tag *tag = (CONST Mb2Tag *)(info + offset);
    if (tag->type == MB2_TAG_END || tag->size < sizeof(Mb2Tag))
      break;
    CONST UINT8 *body = (CONST UINT8 *)tag;

    switch (tag->type) {
    case MB2_TAG_CMDLINE:
      boot_info.cmdline = (CONST CHAR8 *)(body + 8);
      break;
    case MB2_TAG_MODULE:
      if (boot_info.module_count < EFIEMU_MAX_MODULES) {
        EfiEmuModule *m = &boot_info.modules[boot_info.module_count++];
        UINT32 start = *(UINT32 *)(body + 8), end = *(UINT32 *)(body + 12);
        m->start = start;
        m->size = end > start ? end - start : 0;
        m->name = (CONST CHAR8 *)(body + 16);
      }
      break;
    case MB2_TAG_MMAP: {
      UINT32 entry_size = *(UINT32 *)(body + 8);
      if (entry_size < sizeof(Mb2MmapEntry))
        break;
      for (UINT32 e = 16; e + entry_size <= tag->size &&
                          boot_info.memory_count < EFIEMU_MAX_MEMORY_RANGES;
           e += entry_size) {
        CONST Mb2MmapEntry *entry = (CONST Mb2MmapEntry *)(body + e);
        EfiEmuMemoryRange *r = &boot_info.memory[boot_info.memory_count++];
        r->base = entry->base;
        r->length = entry->length;
        r->type = entry->type;
      }
      break;
    }
    case MB2_TAG_FRAMEBUFFER:
      parse_framebuffer(body);
      break;
    case MB2_TAG_EFI64_SYSTEM_TABLE:
      boot_info.efi_system_table = *(UINT64 *)(body + 8);
      break;
    case MB2_TAG_ACPI_OLD:
    case MB2_TAG_ACPI_NEW:
      /* The new (v2) RSDP wins when both are present. */
      if (!boot_info.rsdp || tag->type == MB2_TAG_ACPI_NEW) {
        boot_info.rsdp = (VOID *)(body + 8);
        boot_info.rsdp_length = tag->size - 8;
      }
      break;
    default:
      break;
    }
    offset += (tag->size + 7) & ~7U;
  }

  if (boot_info.memory_count == 0) {
    efiemu_debug_string("xnu-loader kernel: GRUB gave no memory map\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  efiemu_main(&boot_info);
}
