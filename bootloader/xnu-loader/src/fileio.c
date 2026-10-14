#include "fileio.h"
#include "app.h"
#include <efi/efipxebc.h>

EFI_STATUS file_open(
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    UINT64 mode,
    UINT64 attrs,
    EFI_FILE_PROTOCOL **out_file) {
  return uefi_call_wrapper(root->Open, 5, root, out_file, (CHAR16 *)path, mode, attrs);
}

EFI_STATUS file_get_size(EFI_FILE_PROTOCOL *file, UINTN *out_size) {
  EFI_STATUS status;
  UINTN info_size = SIZE_OF_EFI_FILE_INFO + 256;
  EFI_FILE_INFO *info = NULL;

  status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_size, (VOID **)&info);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(
      file->GetInfo,
      4,
      file,
      &gEfiFileInfoGuid,
      &info_size,
      info);
  if (!EFI_ERROR(status))
    *out_size = (UINTN)info->FileSize;

  uefi_call_wrapper(BS->FreePool, 1, info);
  return status;
}

EFI_STATUS file_read_all(
    AppContext *ctx,
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    FileBuffer *out_buf) {
  EFI_STATUS status;
  EFI_FILE_PROTOCOL *file = NULL;
  UINTN size = 0;
  VOID *buffer = NULL;
  UINTN read_size;

  if (!out_buf)
    return EFI_INVALID_PARAMETER;

  out_buf->data = NULL;
  out_buf->size = 0;

  status = file_open(root, path, EFI_FILE_MODE_READ, 0, &file);
  if (EFI_ERROR(status))
    return status;

  status = file_get_size(file, &size);
  if (EFI_ERROR(status))
    goto done;

  status = app_alloc_pool(ctx, size, &buffer);
  if (EFI_ERROR(status))
    goto done;

  read_size = size;
  status = uefi_call_wrapper(file->Read, 3, file, &read_size, buffer);
  if (EFI_ERROR(status))
    goto done;

  out_buf->data = buffer;
  out_buf->size = read_size;
  buffer = NULL;

done:
  if (buffer)
    app_free_pool(ctx, buffer);
  if (file)
    uefi_call_wrapper(file->Close, 1, file);
  return status;
}

VOID file_free(AppContext *ctx, FileBuffer *buf) {
  if (!buf)
    return;
  if (buf->data)
    app_free_pool(ctx, buf->data);
  buf->data = NULL;
  buf->size = 0;
}

EFI_STATUS file_read_all_from_any_volume(
    AppContext *ctx,
    CONST CHAR16 *path,
    FileBuffer *out_buf,
    EFI_HANDLE *out_handle)
{
  EFI_STATUS status;
  EFI_HANDLE *handles = NULL;
  UINTN handle_count = 0;
  UINTN i;

  if (!ctx || !path || !out_buf)
    return EFI_INVALID_PARAMETER;

  out_buf->data = NULL;
  out_buf->size = 0;

  if (out_handle)
    *out_handle = NULL;

  status = uefi_call_wrapper(
      ctx->bs->LocateHandleBuffer,
      5,
      ByProtocol,
      &gEfiSimpleFileSystemProtocolGuid,
      NULL,
      &handle_count,
      &handles);
  if (EFI_ERROR(status))
    return status;

  for (i = 0; i < handle_count; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS try_status;

    try_status = uefi_call_wrapper(
        ctx->bs->HandleProtocol,
        3,
        handles[i],
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&fs);
    if (EFI_ERROR(try_status))
      continue;

    try_status = uefi_call_wrapper(
        fs->OpenVolume,
        2,
        fs,
        &root);
    if (EFI_ERROR(try_status))
      continue;

    try_status = file_read_all(ctx, root, path, out_buf);

    uefi_call_wrapper(root->Close, 1, root);

    if (!EFI_ERROR(try_status)) {
      if (out_handle)
        *out_handle = handles[i];

      uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
      return EFI_SUCCESS;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
  return EFI_NOT_FOUND;
}

EFI_STATUS file_read_all_via_tftp(
    AppContext *ctx,
    CONST CHAR8 *filename,
    FileBuffer *out_buf) {
  EFI_STATUS status;
  EFI_HANDLE *handles = NULL;
  UINTN handle_count = 0;
  EFI_PXE_BASE_CODE_PROTOCOL *pxe = NULL;
  EFI_IP_ADDRESS server_ip;
  UINT64 file_size = 0;
  VOID *buffer = NULL;
  UINTN i;

  if (!ctx || !filename || !out_buf)
    return EFI_INVALID_PARAMETER;

  out_buf->data = NULL;
  out_buf->size = 0;

  status = uefi_call_wrapper(
      ctx->bs->LocateHandleBuffer,
      5,
      ByProtocol,
      &gEfiPxeBaseCodeProtocolGuid,
      NULL,
      &handle_count,
      &handles);
  if (EFI_ERROR(status))
    return EFI_NOT_FOUND;

  for (i = 0; i < handle_count && !pxe; i++) {
    EFI_PXE_BASE_CODE_PROTOCOL *candidate = NULL;
    if (EFI_ERROR(uefi_call_wrapper(
            ctx->bs->HandleProtocol,
            3,
            handles[i],
            &gEfiPxeBaseCodeProtocolGuid,
            (VOID **)&candidate)))
      continue;
    pxe = candidate;
  }
  uefi_call_wrapper(ctx->bs->FreePool, 1, handles);

  if (!pxe)
    return EFI_NOT_FOUND;

  if (!pxe->Mode->Started) {
    status = uefi_call_wrapper(pxe->Start, 2, pxe, FALSE);
    if (EFI_ERROR(status))
      return status;
  }

  if (!pxe->Mode->DhcpAckReceived) {
    /* Firmware netbooted us, so it already ran DHCP itself; if for some
     * reason no ack was captured there is no server IP to target. */
    return EFI_NOT_FOUND;
  }

  SetMem(&server_ip, sizeof(server_ip), 0);
  CopyMem(&server_ip.v4, pxe->Mode->DhcpAck.Dhcpv4.BootpSiAddr, 4);

  status = uefi_call_wrapper(
      pxe->Mtftp,
      10,
      pxe,
      EFI_PXE_BASE_CODE_TFTP_GET_FILE_SIZE,
      NULL,
      FALSE,
      &file_size,
      NULL,
      &server_ip,
      (UINT8 *)filename,
      NULL,
      FALSE);
  if (EFI_ERROR(status) || file_size == 0)
    return EFI_ERROR(status) ? status : EFI_NOT_FOUND;

  status = app_alloc_pool(ctx, (UINTN)file_size, &buffer);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(
      pxe->Mtftp,
      10,
      pxe,
      EFI_PXE_BASE_CODE_TFTP_READ_FILE,
      buffer,
      FALSE,
      &file_size,
      NULL,
      &server_ip,
      (UINT8 *)filename,
      NULL,
      FALSE);
  if (EFI_ERROR(status)) {
    app_free_pool(ctx, buffer);
    return status;
  }

  out_buf->data = buffer;
  out_buf->size = (UINTN)file_size;
  return EFI_SUCCESS;
}