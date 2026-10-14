#include "common.h"
#include "app.h"
#include "console.h"
#include "fileio.h"

// the kernel leaves the tail of a panic log in this variable, the loader turns it into a file on the ESP
// 0f1e4b6a-7c3d-4e2f-9a5b-504450414e49, matching pd_panic_save_efi in xnu's model_dep.c
static EFI_GUID pd_panic_guid = { 0x0f1e4b6a, 0x7c3d, 0x4e2f, { 0x9a, 0x5b, 0x50, 0x44, 0x50, 0x41, 0x4e, 0x49 } };
static CHAR16 pd_panic_name[] = L"PDPanicLog";

#define PD_PANIC_MAX (16 * 1024)

// the root of the volume the loader runs from, found by the file it is always next to
static EFI_FILE_PROTOCOL *panic_boot_root(AppContext *ctx) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_FILE_PROTOCOL *root = NULL;
  EFI_HANDLE handle = NULL;
  FileBuffer probe = {0};

  if (EFI_ERROR(file_read_all_from_any_volume(ctx, L"\\EFI\\BOOT\\boot-args.txt", &probe, &handle)) || !handle)
    return NULL;
  file_free(ctx, &probe);
  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3, handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs)))
    return NULL;
  if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root)))
    return NULL;
  return root;
}

static VOID panic_make_dir(EFI_FILE_PROTOCOL *root, CONST CHAR16 *path) {
  EFI_FILE_PROTOCOL *dir = NULL;

  if (!EFI_ERROR(file_open(root, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                           EFI_FILE_DIRECTORY, &dir)))
    uefi_call_wrapper(dir->Close, 1, dir);
}

// writes a waiting panic log to \EFI\PureDarwin\Panics\panic-N.txt and clears the variable
VOID boot_save_panic_log(AppContext *ctx) {
  EFI_RUNTIME_SERVICES *rt = ST ? ST->RuntimeServices : NULL;
  EFI_FILE_PROTOCOL *root, *file = NULL;
  CHAR16 path[64];
  UINT8 *data = NULL;
  UINT32 attrs = 0;
  UINTN size = PD_PANIC_MAX, n;

  if (!rt || EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiLoaderData, size, (VOID **)&data)))
    return;
  if (EFI_ERROR(uefi_call_wrapper(rt->GetVariable, 5, pd_panic_name, &pd_panic_guid, &attrs, &size, data)) || size == 0) {
    app_free_pool(ctx, data);
    return;
  }

  root = panic_boot_root(ctx);
  if (root) {
    panic_make_dir(root, L"\\EFI\\PureDarwin");
    panic_make_dir(root, L"\\EFI\\PureDarwin\\Panics");
    // the first name not already taken, so earlier logs are kept
    for (n = 1; n < 1000; n++) {
      SPrint(path, sizeof(path), L"\\EFI\\PureDarwin\\Panics\\panic-%d.txt", n);
      if (EFI_ERROR(file_open(root, path, EFI_FILE_MODE_READ, 0, &file)))
        break;
      uefi_call_wrapper(file->Close, 1, file);
    }
    if (!EFI_ERROR(file_open(root, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &file))) {
      UINTN written = size;

      uefi_call_wrapper(file->Write, 3, file, &written, data);
      uefi_call_wrapper(file->Close, 1, file);
      log_info(L"panic log: saved %d bytes to %s\r\n", written, path);
    }
    uefi_call_wrapper(root->Close, 1, root);
  }

  // cleared either way, a log that could not be written must not repeat on every boot
  uefi_call_wrapper(rt->SetVariable, 5, pd_panic_name, &pd_panic_guid, attrs, (UINTN)0, NULL);
  app_free_pool(ctx, data);
}
