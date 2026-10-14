#ifndef RISCV_EFI_BOOT_H
#define RISCV_EFI_BOOT_H

#include <efi.h>

// the uefi risc-v boot protocol, how an efi application learns which hart booted it
#define RISCV_EFI_BOOT_PROTOCOL_GUID \
  { 0xccd15fec, 0x6f73, 0x4eec, { 0x83, 0x95, 0x3e, 0x69, 0xe4, 0xb9, 0x40, 0xbf } }
#define RISCV_EFI_BOOT_PROTOCOL_REVISION 0x00010000ULL

typedef struct _RISCV_EFI_BOOT_PROTOCOL {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *GetBootHartId)(struct _RISCV_EFI_BOOT_PROTOCOL *This, UINTN *BootHartId);
} RISCV_EFI_BOOT_PROTOCOL;

#endif
