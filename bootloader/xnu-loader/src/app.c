#include "app.h"
#include "console.h"
#include "serial.h"
#include "platform.h"

EFI_STATUS app_init(AppContext *ctx, EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  if (!st)
    return EFI_INVALID_PARAMETER;

  if (!ctx || st->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE)
    return EFI_ABORTED;

  // ensure there's no leftover data. this caused a bug on some machines
  // to falsely report having a RAMDisk when it did not.
  SetMem(ctx, sizeof(*ctx), 0);

  ctx->image_handle = image;
  ctx->st = st;
  ctx->bs = st->BootServices;
  ctx->rt = st->RuntimeServices;

  InitializeLib(image, st);

#if defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_GENERIC)
  // the console and the rest of the board come from the firmware's device tree, else the build's defaults
  fdt_board_parse(fdt_board_find(st));
#endif

  /* Bring up the serial console before any logging so serial captures the full
   * boot on real hardware (where there is no EFI console to read). */
  serial_init();

  return EFI_SUCCESS;
}

EFI_STATUS app_get_loaded_image(AppContext *ctx, EFI_LOADED_IMAGE **loaded_image) {
  return uefi_call_wrapper(
      ctx->bs->HandleProtocol,
      3,
      ctx->image_handle,
      &LoadedImageProtocol,
      (VOID **)loaded_image);
}

EFI_STATUS app_open_self_volume(AppContext *ctx, EFI_FILE_PROTOCOL **root) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE *loaded_image = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;

  status = app_get_loaded_image(ctx, &loaded_image);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(
      ctx->bs->HandleProtocol,
      3,
      loaded_image->DeviceHandle,
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID **)&fs);
  if (EFI_ERROR(status))
    return status;

  return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
}

EFI_STATUS app_alloc_pool(AppContext *ctx, UINTN size, VOID **ptr) {
  return uefi_call_wrapper(
      ctx->bs->AllocatePool,
      3,
      EfiLoaderData,
      size,
      ptr);
}

VOID app_free_pool(AppContext *ctx, VOID *ptr) {
  if (ptr)
    uefi_call_wrapper(ctx->bs->FreePool, 1, ptr);
}

static UINT64 app_detect_memory(AppContext *ctx, UINT64 base, BOOLEAN extent) {
  UINTN map_size = 0, key = 0, desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_MEMORY_DESCRIPTOR *mm = NULL;
  UINT64 total = 0, end = base;

  uefi_call_wrapper(ctx->bs->GetMemoryMap, 5,
      &map_size, mm, &key, &desc_size, &desc_ver);
  map_size += desc_size * 4;

  EFI_STATUS s = uefi_call_wrapper(ctx->bs->AllocatePool, 3,
      EfiLoaderData, map_size, (VOID **)&mm);
  if (EFI_ERROR(s) || !mm)
    return 0;

  s = uefi_call_wrapper(ctx->bs->GetMemoryMap, 5,
      &map_size, mm, &key, &desc_size, &desc_ver);
  if (EFI_ERROR(s)) {
    uefi_call_wrapper(ctx->bs->FreePool, 1, mm);
    return 0;
  }

  UINTN n = map_size / desc_size;
  for (UINTN i = 0; i < n; i++) {
    EFI_MEMORY_DESCRIPTOR *d =
        (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mm + i * desc_size);
    if (d->Type == EfiLoaderCode         ||
        d->Type == EfiLoaderData         ||
        d->Type == EfiBootServicesCode   ||
        d->Type == EfiBootServicesData   ||
        d->Type == EfiRuntimeServicesCode ||
        d->Type == EfiRuntimeServicesData ||
        d->Type == EfiConventionalMemory ||
        d->Type == EfiACPIReclaimMemory  ||
        d->Type == EfiACPIMemoryNVS      ||
        d->Type == EfiPalCode) {
      UINT64 bytes = d->NumberOfPages << EFI_PAGE_SHIFT;
      total += bytes;
      if (d->PhysicalStart >= base && bytes <= ~0ULL - d->PhysicalStart &&
          d->PhysicalStart + bytes > end)
        end = d->PhysicalStart + bytes;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, mm);
  return extent ? end - base : total;
}

UINT64 app_detect_physical_memory_size(AppContext *ctx) {
  return app_detect_memory(ctx, 0, FALSE);
}

UINT64 app_detect_physical_memory_extent(AppContext *ctx, UINT64 base) {
  return app_detect_memory(ctx, base, TRUE);
}
