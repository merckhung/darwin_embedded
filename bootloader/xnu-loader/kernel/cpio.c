#include "efi_emulation.h"
#include <efilib.h>

/* newc cpio archive -> modules, shared by every boot protocol */
static UINT32 hex8(CONST UINT8 *s) {
  UINT32 v = 0;
  for (UINTN i = 0; i < 8; ++i) {
    UINT8 c = s[i];
    UINT32 d = c >= '0' && c <= '9' ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : 0;
    v = v << 4 | d;
  }
  return v;
}

void kernel_parse_cpio(EfiEmuBootInfo *info, UINT64 start, UINT64 size) {
  CONST UINT8 *base = (CONST UINT8 *)(UINTN)start;
  UINT64 off = 0;
  while (off + 110 <= size && info->module_count < EFIEMU_MAX_MODULES) {
    CONST UINT8 *h = base + off;
    if (CompareMem(h, "070701", 6) != 0 && CompareMem(h, "070702", 6) != 0) {
      efiemu_debug_string("xnu-loader kernel: initrd is not a newc cpio archive\n");
      return;
    }
    UINT32 mode = hex8(h + 14);
    UINT32 file_size = hex8(h + 54);
    UINT32 name_size = hex8(h + 94);
    CONST CHAR8 *name = (CONST CHAR8 *)(h + 110);
    UINT64 data = (off + 110 + name_size + 3) & ~3ULL;
    if (name_size == 0 || data + file_size > size)
      return;
    if (name_size == 11 && CompareMem(name, "TRAILER!!!", 10) == 0)
      return;
    if ((mode & 0170000) == 0100000) {
      EfiEmuModule *m = &info->modules[info->module_count++];
      m->start = start + data;
      m->size = file_size;
      m->name = name; /* NUL-terminated inside the archive */
    }
    off = (data + file_size + 3) & ~3ULL;
  }
}

