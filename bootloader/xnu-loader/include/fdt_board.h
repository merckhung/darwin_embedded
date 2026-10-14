#ifndef XNU_LOADER_FDT_BOARD_H
#define XNU_LOADER_FDT_BOARD_H

#include <efi.h>

// what the loader needs to know about an arm64 board, read from its flattened device tree
typedef enum {
  FDT_UART_NONE = 0,
  FDT_UART_PL011,
  FDT_UART_16550,
} FdtUartKind;

typedef struct {
  BOOLEAN from_fdt;
  BOOLEAN is_qemu_virt;
  BOOLEAN has_pci_ecam;

  UINT64 ram_base;

  // the arm-io window XNU sees; zero means derive it from the devices below
  UINT64 soc_base, soc_size;

  FdtUartKind uart_kind;
  UINT64 uart_base, uart_size;
  UINT32 uart_shift, uart_width;
  UINT32 uart_clock, uart_baud;   // clock 0: take it from the divisor firmware left

  UINT32 psci_method;             // 0 unknown, 1 hvc, 2 smc

  UINT32 gic_version;             // 2 or 3, 0 when there is none
  UINT64 gicd_base, gicd_size;
  UINT64 gic2_base, gic2_size;    // gicv2 cpu interface, or the gicv3 redistributor region
} FdtBoard;

extern FdtBoard g_board;

// the build's defaults, used as-is when there is no fdt
VOID fdt_board_defaults(VOID);
BOOLEAN fdt_board_parse(CONST VOID *fdt);
CONST VOID *fdt_board_find(EFI_SYSTEM_TABLE *st);

#endif
