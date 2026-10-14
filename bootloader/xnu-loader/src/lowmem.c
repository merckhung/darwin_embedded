#include "lowmem.h"

static UINTN lowmem_pages_for_size(UINTN size) {
  return (size + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT;
}

EFI_STATUS lowmem_alloc_pages(
    AppContext *ctx,
    UINTN size,
    EFI_MEMORY_TYPE type,
    LowMemBuffer *out_buf)
{
  EFI_STATUS status;
  EFI_PHYSICAL_ADDRESS addr;
  UINTN pages;

  if (!ctx || !out_buf || size == 0)
    return EFI_INVALID_PARAMETER;

  pages = lowmem_pages_for_size(size);
  addr = 0xFFFFFFFFULL;

  out_buf->ptr = NULL;
  out_buf->phys = 0;
  out_buf->size = 0;
  out_buf->pages = 0;

  status = uefi_call_wrapper(
      ctx->bs->AllocatePages,
      4,
      AllocateMaxAddress,
      type,
      pages,
      &addr);
  if (EFI_ERROR(status))
    return status;

  SetMem((VOID *)(UINTN)addr, pages << EFI_PAGE_SHIFT, 0);

  out_buf->ptr = (VOID *)(UINTN)addr;
  out_buf->phys = addr;
  out_buf->size = size;
  out_buf->pages = pages;
  return EFI_SUCCESS;
}

EFI_STATUS lowmem_realloc_copy(
    AppContext *ctx,
    VOID *src,
    UINTN size,
    EFI_MEMORY_TYPE type,
    LowMemBuffer *out_buf)
{
  EFI_STATUS status;

  if (!ctx || !src || !out_buf || size == 0)
    return EFI_INVALID_PARAMETER;

  status = lowmem_alloc_pages(ctx, size, type, out_buf);
  if (EFI_ERROR(status))
    return status;

  CopyMem(out_buf->ptr, src, size);
  return EFI_SUCCESS;
}

VOID lowmem_free(
    AppContext *ctx,
    LowMemBuffer *buf)
{
  if (!ctx || !buf)
    return;

  if (buf->ptr != NULL && buf->pages != 0) {
    uefi_call_wrapper(
        ctx->bs->FreePages,
        2,
        buf->phys,
        buf->pages);
  }

  buf->ptr = NULL;
  buf->phys = 0;
  buf->size = 0;
  buf->pages = 0;
}