#ifndef FILEIO_H
#define FILEIO_H

#include "common.h"
#include "app.h"

EFI_STATUS file_open(
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    UINT64 mode,
    UINT64 attrs,
    EFI_FILE_PROTOCOL **out_file);

EFI_STATUS file_get_size(
    EFI_FILE_PROTOCOL *file,
    UINTN *out_size);

EFI_STATUS file_read_all(
    AppContext *ctx,
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    FileBuffer *out_buf);

VOID file_free(
    AppContext *ctx,
    FileBuffer *buf);

EFI_STATUS file_read_all_from_any_volume(
    AppContext *ctx,
    CONST CHAR16 *path,
    FileBuffer *out_buf,
    EFI_HANDLE *out_handle);

/* Netboot fallback: fetch `filename` (ASCII, relative to the TFTP server
 * root - same root BOOTX64.EFI itself was served from) via the
 * EFI_PXE_BASE_CODE_PROTOCOL instance the firmware installs on the image's
 * own device handle when it was PXE-booted. Only usable when the firmware
 * actually netbooted us; returns EFI_NOT_FOUND if no PXE Base Code protocol
 * instance is present at all (e.g. booted from local media). */
EFI_STATUS file_read_all_via_tftp(
    AppContext *ctx,
    CONST CHAR8 *filename,
    FileBuffer *out_buf);

#endif