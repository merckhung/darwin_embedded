#include "efi_emulation.h"
#include "serial.h"

/* Linux x86 boot protocol: boot_params ("zero page") -> EfiEmuBootInfo. The
 * single initrd is a cpio (newc) archive whose regular files become modules,
 * e.g. EFI/BOOT/kernel and EFI/BOOT/boot-args.txt. */

#define BP_SCREEN_INFO 0x000
#define BP_ACPI_RSDP_ADDR 0x070
#define BP_EFI_INFO 0x1c0
#define BP_EXT_RAMDISK_IMAGE 0x0c0
#define BP_EXT_RAMDISK_SIZE 0x0c4
#define BP_EXT_CMD_LINE_PTR 0x0c8
#define BP_E820_ENTRIES 0x1e8
#define BP_RAMDISK_IMAGE 0x218
#define BP_RAMDISK_SIZE 0x21c
#define BP_CMD_LINE_PTR 0x228
#define BP_E820_TABLE 0x2d0
#define BP_E820_MAX 128

/* efi_info.efi_loader_signature from a 64-bit UEFI bootloader. */
#define EFI_LOADER_SIGNATURE_64 0x34364c45 /* "EL64" */

#define VIDEO_TYPE_VLFB 0x23
#define VIDEO_TYPE_EFI 0x70
#define VIDEO_CAPABILITY_64BIT_BASE (1U << 1)

static EfiEmuBootInfo boot_info;
static CHAR8 cmdline[2048];

static UINT8 rd8(CONST UINT8 *p, UINTN o) { return p[o]; }
static UINT16 rd16(CONST UINT8 *p, UINTN o) { return (UINT16)(p[o] | p[o + 1] << 8); }
static UINT32 rd32(CONST UINT8 *p, UINTN o) {
  return (UINT32)p[o] | (UINT32)p[o + 1] << 8 | (UINT32)p[o + 2] << 16 |
         (UINT32)p[o + 3] << 24;
}
static UINT64 rd64(CONST UINT8 *p, UINTN o) {
  return (UINT64)rd32(p, o) | (UINT64)rd32(p, o + 4) << 32;
}

static void parse_screen_info(CONST UINT8 *bp) {
  CONST UINT8 *si = bp + BP_SCREEN_INFO;
  UINT8 type = rd8(si, 0x0f);
  if ((type != VIDEO_TYPE_VLFB && type != VIDEO_TYPE_EFI) || rd16(si, 0x16) != 32)
    return;
  UINT64 base = rd32(si, 0x18);
  if (rd32(si, 0x36) & VIDEO_CAPABILITY_64BIT_BASE)
    base |= (UINT64)rd32(si, 0x3a) << 32;
  UINT16 line = rd16(si, 0x24);
  UINT16 height = rd16(si, 0x14);
  if (base == 0 || base + (UINT64)line * height > 0x100000000ULL)
    return;
  EfiEmuFramebuffer *fb = &boot_info.framebuffer;
  fb->base = base;
  fb->width = rd16(si, 0x12);
  fb->height = height;
  /* VLFB counts line length in bytes; the EFI type already stores pixels. */
  fb->pixels_per_scanline = type == VIDEO_TYPE_VLFB ? line / 4 : line;
  fb->bits_per_pixel = 32;
  fb->red_position = rd8(si, 0x27);
  fb->blue_position = rd8(si, 0x2b);
  fb->valid = 1;
}

/* Copy the command line without GRUB's BOOT_IMAGE=... token. */
static void copy_cmdline(CONST CHAR8 *src) {
  UINTN out = 0;
  while (src && *src) {
    while (*src == ' ')
      ++src;
    CONST CHAR8 *word = src;
    while (*src && *src != ' ')
      ++src;
    UINTN len = (UINTN)(src - word);
    if (len == 0)
      break;
    BOOLEAN skip = len >= 11 && CompareMem(word, "BOOT_IMAGE=", 11) == 0;
    if (skip)
      continue;
    if (out && out < sizeof(cmdline) - 1)
      cmdline[out++] = ' ';
    for (UINTN i = 0; i < len && out < sizeof(cmdline) - 1; ++i)
      cmdline[out++] = word[i];
  }
  cmdline[out] = 0;
}

extern CONST CHAR8 embedded_initrd_start[], embedded_initrd_end[];

void kernel_linux_main(UINT64 boot_params) {
  CONST UINT8 *bp = (CONST UINT8 *)(UINTN)boot_params;
  efiemu_debug_string("xnu-loader kernel: linux boot protocol entry\n");

  boot_info.protocol_data_base = boot_params;
  boot_info.protocol_data_size = 4096;

  UINT8 entries = rd8(bp, BP_E820_ENTRIES);
  if (entries > BP_E820_MAX)
    entries = BP_E820_MAX;
  for (UINT8 i = 0; i < entries && boot_info.memory_count < EFIEMU_MAX_MEMORY_RANGES; ++i) {
    CONST UINT8 *e = bp + BP_E820_TABLE + i * 20;
    EfiEmuMemoryRange *r = &boot_info.memory[boot_info.memory_count++];
    r->base = rd64(e, 0);
    r->length = rd64(e, 8);
    r->type = rd32(e, 16);
  }

  UINT64 cmd = rd32(bp, BP_CMD_LINE_PTR) | (UINT64)rd32(bp, BP_EXT_CMD_LINE_PTR) << 32;
  copy_cmdline(cmd ? (CONST CHAR8 *)(UINTN)cmd : NULL);
  if (cmdline[0])
    boot_info.cmdline = cmdline;

  UINT64 rsdp = rd64(bp, BP_ACPI_RSDP_ADDR);
  if (rsdp)
    boot_info.rsdp = (VOID *)(UINTN)rsdp;

  parse_screen_info(bp);

  /* efi_info: systab at +4, systab_hi at +0x18. */
  if (rd32(bp, BP_EFI_INFO) == EFI_LOADER_SIGNATURE_64)
    boot_info.efi_system_table =
        rd32(bp, BP_EFI_INFO + 4) | (UINT64)rd32(bp, BP_EFI_INFO + 0x18) << 32;

  UINT64 initrd = rd32(bp, BP_RAMDISK_IMAGE) | (UINT64)rd32(bp, BP_EXT_RAMDISK_IMAGE) << 32;
  UINT64 initrd_size = rd32(bp, BP_RAMDISK_SIZE) | (UINT64)rd32(bp, BP_EXT_RAMDISK_SIZE) << 32;
  /* A built-in payload wins: hosts like WSL always pass an initrd of their own. */
  if (embedded_initrd_end != embedded_initrd_start) {
    initrd = (UINT64)(UINTN)embedded_initrd_start;
    initrd_size = (UINT64)(embedded_initrd_end - embedded_initrd_start);
    efiemu_debug_string("xnu-loader kernel: using the built-in initrd\n");
  }
  if (initrd && initrd_size)
    kernel_parse_cpio(&boot_info, initrd, initrd_size);
  else
    efiemu_debug_string("xnu-loader kernel: no initrd\n");

  if (boot_info.memory_count == 0) {
    efiemu_debug_string("xnu-loader kernel: boot_params has no E820 map\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  efiemu_main(&boot_info);
}
