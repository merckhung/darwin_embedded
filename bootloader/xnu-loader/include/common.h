#ifndef COMMON_H
#define COMMON_H

#include <efi.h>
#include <efilib.h>
#include <string.h>

/* Firmware CPU family. A 32-bit UEFI implementation runs on the same x86 CPU
 * as a 64-bit one, so port I/O, cli/sti and rdtsc are shared - only pointer
 * width and the kernel handoff differ. Kept separate from CONFIG_x86_64, which
 * describes the kernel being booted rather than the code doing the booting. */
#if defined(__x86_64__) || defined(__i386__)
#define PD_ARCH_X86 1
#endif

//#define VERBOSE_MACHO
//#define VERBOSE_BOOT
//#define KASLR_ENABLED

/*
 * Boot-info block layout (physical addresses).
 *
 * The kernel image lands at [0x100000, ~0x1779000); this block sits above it.
 * Runtime-services VAs are packed above it (see boot.c SVAM) starting at
 * XNU_RT_VA_BASE; ksize is inflated so physfree covers everything.
 *
 * Layout (phys):
 *   [0x100000, ~0x28bc000)   kernel image (fileset KC extends this far)
 *   [0x3000000, 0x3020000)   boot-info block (boot_args/tables/DT/memmap)
 *   [0x3200000, va_cursor)   runtime-services VA pack; physfree = round_up(va_cursor)
 */
#if defined(__aarch64__) || defined(__riscv)
extern EFI_PHYSICAL_ADDRESS g_xnu_bootinfo_base;
#define XNU_BOOTINFO_BASE       g_xnu_bootinfo_base
#else
#define XNU_BOOTINFO_BASE       0x3000000ULL   /* 2MB-aligned, above KC image  */
#endif
#if defined(__riscv)
// riscv64 xnu runs on 4k pages
#define XNU_BOOTINFO_ALIGN      0x1000ULL
#define XNU_L2_BLOCK_SIZE       0x200000ULL
#else
// 4k-page kernels: the cortex-a53 boards, or any generic board built with XNU_LOADER_KERNEL_4K
#if defined(XNU_LOADER_KERNEL_4K) || defined(XNU_LOADER_PLATFORM_SUN50I) || defined(XNU_LOADER_PLATFORM_SG2002)
#define XNU_BOOTINFO_ALIGN      0x1000ULL
#define XNU_L2_BLOCK_SIZE       0x200000ULL
#else
#define XNU_BOOTINFO_ALIGN      0x4000ULL
#define XNU_L2_BLOCK_SIZE       0x2000000ULL
#endif
#endif

#define XNU_BOOTARGS_PHYS       (XNU_BOOTINFO_BASE + 0x00000) /* 1 page       */
#define XNU_EFITABLES_PHYS      (XNU_BOOTINFO_BASE + 0x01000) /* 1 page       */
/* 8 pages: /chosen carries an nvram-proxy-data blob the size of one NVRAM
 * bank, which alone is twice the old 2-page budget. Sits in the hole between
 * the trustcache page and the memory map rather than growing in place, so
 * XNU_ARM64_BOOTARGS_PHYS and XNU_TRUSTCACHE_PHYS keep their addresses. */
#if defined(__aarch64__)
// a board's own devices come over from its fdt, so arm64 puts the tree after the memory map
#define XNU_DEVTREE_PHYS        (XNU_BOOTINFO_BASE + 0x20000) /* 64 pages     */
#define XNU_DEVTREE_PAGES       64
#else
#define XNU_DEVTREE_PHYS        (XNU_BOOTINFO_BASE + 0x06000) /* 8 pages      */
#if defined(__riscv)
// the whole fdt is carried over, so the tree runs up to the end of the block
#define XNU_DEVTREE_PAGES       24
#else
#define XNU_DEVTREE_PAGES       8
#endif
#endif
#if defined(__aarch64__) || defined(__riscv)
#define XNU_ARM64_BOOTARGS_PHYS (XNU_BOOTINFO_BASE + 0x04000) /* 1 page       */
#endif
#define XNU_TRUSTCACHE_PHYS     (XNU_BOOTINFO_BASE + 0x05000) /* 1 page       */
#define XNU_MEMMAP_PHYS         (XNU_BOOTINFO_BASE + 0x10000) /* up to 16 pg  */
#if defined(__aarch64__)
#define XNU_BOOTINFO_END        (XNU_BOOTINFO_BASE + 0x60000) /* 384KB block  */
#else
#define XNU_BOOTINFO_END        (XNU_BOOTINFO_BASE + 0x20000) /* 128KB block  */
#endif
#define XNU_RT_VA_BASE          0x3200000ULL   /* runtime-services VA pack base */

/*
 * GNU-EFI 4.x marks CopyMem_1 and SetMem as EFIAPI (__attribute__((ms_abi))),
 * meaning they expect arguments in RCX/RDX/R8 (Windows calling convention).
 * Our code is compiled with the SysV ABI (RDI/RSI/RDX), so every direct call
 * to CopyMem/SetMem from our code has an ABI mismatch and silently does nothing.
 *
 * Override both macros with inline wrappers that use the correct calling convention.
 */
#undef CopyMem
#undef SetMem

static inline VOID *XnuCopyMem(VOID *dst, const VOID *src, UINTN n) {
  UINT8 *d = (UINT8 *)dst;
  const UINT8 *s = (const UINT8 *)src;
  while (n--) *d++ = *s++;
  return dst;
}
static inline VOID *XnuSetMem(VOID *dst, UINTN n, UINT8 val) {
  UINT8 *d = (UINT8 *)dst;
  while (n--) *d++ = val;
  return dst;
}

#define CopyMem(dst, src, n) XnuCopyMem((VOID *)(UINTN)(dst), (const VOID *)(UINTN)(src), (UINTN)(n))
#define SetMem(dst, n, val) XnuSetMem((VOID *)(UINTN)(dst), (UINTN)(n), (UINT8)(val))

typedef struct AppContext {
  EFI_HANDLE image_handle;
  EFI_SYSTEM_TABLE *st;
  EFI_BOOT_SERVICES *bs;
  EFI_RUNTIME_SERVICES *rt;
  EFI_HANDLE boot_volume;
  UINT32 kslide;
  UINT64 phys_base;
  EFI_PHYSICAL_ADDRESS kernel_region_base;
  EFI_PHYSICAL_ADDRESS kernel_region_end;
  /* Page-rounded RAMDisk retained through ExitBootServices. */
  EFI_PHYSICAL_ADDRESS ramdisk_phys;
  UINT64 ramdisk_size;
  /* Same calibrated bus frequency in the EFI tree and x86 boot arguments. */
  UINT64 fsb_frequency;
#if defined(__aarch64__)
  /* Release XNU expects the trust-cache EXTRADATA range below the KC. */
  EFI_PHYSICAL_ADDRESS trustcache_phys;
  // \trustcache.bin from the ESP (offsets header + modules), and the slot reserved below the KC for it
  VOID *trustcache_data;
  UINT64 trustcache_size;
  UINT64 trustcache_slack;
#endif
#if defined(__riscv)
  // the ram bank holding the kernel, and the fdt copy placed next to the kernel
  UINT64 dram_base;
  UINT64 dram_size;
  EFI_PHYSICAL_ADDRESS fdt_copy_phys;
  UINT64 fdt_copy_size;
  // from RISCV_EFI_BOOT_PROTOCOL, the kernel gets it in a1
  UINT64 boot_hartid;
#endif
} AppContext;

typedef struct FileBuffer {
  VOID *data;
  UINTN size;
} FileBuffer;

#endif
