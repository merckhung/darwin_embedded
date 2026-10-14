#ifndef LOWMEM_H
#define LOWMEM_H

#include "common.h"
#include "app.h"

typedef struct LowMemBuffer {
  VOID *ptr;
  EFI_PHYSICAL_ADDRESS phys;
  UINTN size;
  UINTN pages;
} LowMemBuffer;

EFI_STATUS lowmem_alloc_pages(
    AppContext *ctx,
    UINTN size,
    EFI_MEMORY_TYPE type,
    LowMemBuffer *out_buf);

EFI_STATUS lowmem_realloc_copy(
    AppContext *ctx,
    VOID *src,
    UINTN size,
    EFI_MEMORY_TYPE type,
    LowMemBuffer *out_buf);

VOID lowmem_free(
    AppContext *ctx,
    LowMemBuffer *buf);

#endif