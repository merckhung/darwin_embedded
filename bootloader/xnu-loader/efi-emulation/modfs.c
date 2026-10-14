#include "efi_emulation.h"
#include <efilib.h>

/* Read-only volume over boot-protocol modules: a module named
 * "/EFI/BOOT/kernel" opens as \EFI\BOOT\kernel. */

#define MAX_OPEN 12

typedef struct {
  EFI_FILE_PROTOCOL proto;
  CONST EfiEmuModule *module; /* NULL for the root directory */
  UINT64 position;
  BOOLEAN allocated;
} ModFile;

static EfiEmuModule modules[EFIEMU_MAX_MODULES + 1];
static UINT32 module_count;
static CHAR8 cmdline_copy[1024];
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
static ModFile root;
static ModFile files[MAX_OPEN];
static EFI_HANDLE volume_handle = (EFI_HANDLE)(UINTN)0x4d4f4453;
static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
static BOOLEAN ready;

static BOOLEAN guid_equal(CONST EFI_GUID *a, CONST EFI_GUID *b) {
  CONST UINT64 *x = (CONST UINT64 *)a, *y = (CONST UINT64 *)b;
  return x[0] == y[0] && x[1] == y[1];
}

static CHAR16 fold(CHAR16 c) {
  if (c == '/')
    return '\\';
  if (c >= 'A' && c <= 'Z')
    return c + 32;
  return c;
}

/* Paths match ignoring case and slash direction, and leading separators. */
static BOOLEAN path_equal(CONST CHAR16 *wide, CONST CHAR8 *narrow) {
  while (*wide == '\\' || *wide == '/')
    ++wide;
  while (*narrow == '\\' || *narrow == '/')
    ++narrow;
  for (;; ++wide, ++narrow) {
    CHAR16 a = fold(*wide), b = fold((UINT8)*narrow);
    if (b == ' ')
      b = 0; /* GRUB may leave arguments after the name */
    if (a != b)
      return FALSE;
    if (a == 0)
      return TRUE;
  }
}

static BOOLEAN name_is(CONST CHAR8 *name, CONST CHAR8 *wanted) {
  CHAR16 wide[64];
  UINTN i = 0;
  for (; wanted[i] && i < 63; ++i)
    wide[i] = (UINT8)wanted[i];
  wide[i] = 0;
  return path_equal(wide, name);
}

static EFI_STATUS EFIAPI mod_close(EFI_FILE_PROTOCOL *p) {
  ModFile *f = (ModFile *)p;
  if (f != &root)
    f->allocated = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mod_read(EFI_FILE_PROTOCOL *p, UINTN *amount, VOID *buffer) {
  ModFile *f = (ModFile *)p;
  if (!amount || !buffer || !f->module)
    return EFI_UNSUPPORTED;
  UINT64 left = f->module->size - f->position;
  UINTN n = *amount < left ? *amount : (UINTN)left;
  CopyMem(buffer, (VOID *)(UINTN)(f->module->start + f->position), n);
  f->position += n;
  *amount = n;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mod_get_info(EFI_FILE_PROTOCOL *p, EFI_GUID *type,
                                      UINTN *amount, VOID *buffer) {
  ModFile *f = (ModFile *)p;
  if (!type || !amount || !guid_equal(type, &file_info_guid))
    return EFI_UNSUPPORTED;
  UINTN need = SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16);
  if (!buffer || *amount < need) {
    *amount = need;
    return EFI_BUFFER_TOO_SMALL;
  }
  EFI_FILE_INFO *info = buffer;
  SetMem(info, need, 0);
  info->Size = need;
  info->FileSize = f->module ? f->module->size : 0;
  info->PhysicalSize = info->FileSize;
  info->Attribute = f->module ? EFI_FILE_READ_ONLY : EFI_FILE_DIRECTORY;
  *amount = need;
  return EFI_SUCCESS;
}

static void init_file(ModFile *f);

static EFI_STATUS EFIAPI mod_open(EFI_FILE_PROTOCOL *p, EFI_FILE_PROTOCOL **out,
                                  CHAR16 *path, UINT64 mode, UINT64 attrs) {
  (void)p;
  (void)attrs;
  if (!out || !path || mode != EFI_FILE_MODE_READ)
    return EFI_UNSUPPORTED;
  for (UINT32 i = 0; i < module_count; ++i) {
    if (!path_equal(path, modules[i].name))
      continue;
    for (UINTN s = 0; s < MAX_OPEN; ++s) {
      if (files[s].allocated)
        continue;
      init_file(&files[s]);
      files[s].allocated = TRUE;
      files[s].module = &modules[i];
      *out = &files[s].proto;
      return EFI_SUCCESS;
    }
    return EFI_OUT_OF_RESOURCES;
  }
  return EFI_NOT_FOUND;
}

static void init_file(ModFile *f) {
  SetMem(f, sizeof(*f), 0);
  f->proto.Revision = EFI_FILE_PROTOCOL_REVISION;
  f->proto.Open = mod_open;
  f->proto.Close = mod_close;
  f->proto.Read = mod_read;
  f->proto.GetInfo = mod_get_info;
}

static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
                                     EFI_FILE_PROTOCOL **out) {
  (void)self;
  if (!out)
    return EFI_INVALID_PARAMETER;
  *out = &root.proto;
  return EFI_SUCCESS;
}

EFI_STATUS efiemu_modfs_init(EfiEmuBootInfo *info) {
  module_count = 0;
  BOOLEAN have_args = FALSE;
  for (UINT32 i = 0; i < info->module_count; ++i) {
    if (!info->modules[i].name || !info->modules[i].name[0])
      continue;
    modules[module_count++] = info->modules[i];
    if (name_is(info->modules[i].name, "/EFI/BOOT/boot-args.txt"))
      have_args = TRUE;
  }

  /* No boot-args module: the kernel's own command line is the boot-args. */
  if (!have_args && info->cmdline && info->cmdline[0]) {
    UINTN n = 0;
    while (info->cmdline[n] && n < sizeof(cmdline_copy) - 1) {
      cmdline_copy[n] = info->cmdline[n];
      ++n;
    }
    cmdline_copy[n] = 0;
    modules[module_count].start = (UINT64)(UINTN)cmdline_copy;
    modules[module_count].size = n;
    modules[module_count].name = (CONST CHAR8 *)"/EFI/BOOT/boot-args.txt";
    ++module_count;
  }

  if (module_count == 0)
    return EFI_NOT_FOUND;

  init_file(&root);
  root.allocated = TRUE;
  fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
  fs.OpenVolume = open_volume;
  ready = TRUE;

  for (UINT32 i = 0; i < module_count; ++i) {
    efiemu_debug_string("efi-emulation: module ");
    efiemu_debug_string((const char *)modules[i].name);
    efiemu_debug_string(" at ");
    efiemu_debug_hex(modules[i].start);
    efiemu_debug_string(" size ");
    efiemu_debug_hex(modules[i].size);
    efiemu_debug_string("\n");
  }
  return EFI_SUCCESS;
}

EFI_HANDLE efiemu_modfs_handle(void) {
  return volume_handle;
}

EFI_STATUS efiemu_modfs_protocol(EFI_GUID *guid, VOID **out) {
  if (!guid || !out)
    return EFI_INVALID_PARAMETER;
  if (!ready || !guid_equal(guid, &fs_guid))
    return EFI_UNSUPPORTED;
  *out = &fs;
  return EFI_SUCCESS;
}
