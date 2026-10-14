#ifndef APP_H
#define APP_H

#include "common.h"
#include "uefi/smbios.h"

EFI_STATUS app_init(AppContext *ctx, EFI_HANDLE image, EFI_SYSTEM_TABLE *st);
EFI_STATUS app_get_loaded_image(AppContext *ctx, EFI_LOADED_IMAGE **loaded_image);
EFI_STATUS app_open_self_volume(AppContext *ctx, EFI_FILE_PROTOCOL **root);
EFI_STATUS app_alloc_pool(AppContext *ctx, UINTN size, VOID **ptr);
VOID app_free_pool(AppContext *ctx, VOID *ptr);
uint64_t app_detect_physical_memory_size(AppContext *ctx);
uint64_t app_detect_physical_memory_extent(AppContext *ctx, uint64_t base);

#endif