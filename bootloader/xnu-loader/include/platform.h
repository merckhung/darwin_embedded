#ifndef XNU_LOADER_PLATFORM_H
#define XNU_LOADER_PLATFORM_H

#if defined(__aarch64__)

/*
 * Two kinds of arm64 target. BCM2837 is described by its own code (VideoCore
 * mailbox, no usable DTB at the loader). Everything else is a generic board read
 * from the flattened device tree the firmware passes (fdt_board.c).
 *
 * qemuvirt, sun50i and sg2002 are generic boards too; the names only choose the
 * defaults used when no DTB is found (EDK2 on QEMU virt boots with ACPI) and keep
 * those boards' arm-io window as it was.
 */
#if defined(XNU_LOADER_PLATFORM_QEMUVIRT) || defined(XNU_LOADER_PLATFORM_SUN50I) || \
    defined(XNU_LOADER_PLATFORM_SG2002) || defined(XNU_LOADER_PLATFORM_SC8280XP)
#undef XNU_LOADER_PLATFORM_GENERIC
#define XNU_LOADER_PLATFORM_GENERIC 1
#endif

#if (defined(XNU_LOADER_PLATFORM_BCM2837) + defined(XNU_LOADER_PLATFORM_GENERIC)) != 1
#error "aarch64 needs exactly one of XNU_LOADER_PLATFORM_BCM2837 or a generic platform"
#endif

// the cortex-a53 boards run a 4k-page kernel; everything else the 16k one
#if defined(XNU_LOADER_PLATFORM_SUN50I) || defined(XNU_LOADER_PLATFORM_SG2002)
#undef XNU_LOADER_KERNEL_4K
#define XNU_LOADER_KERNEL_4K 1
#endif

/*
 * Base of physical DRAM, used both to place the kernel image and to fill
 * /chosen dram-base. BCM2837 starts at 0; generic boards say where in the DTB.
 */
#if defined(XNU_LOADER_PLATFORM_BCM2837)
#define XNU_LOADER_RAM_BASE 0ULL
#else
#include "fdt_board.h"
#define XNU_LOADER_RAM_BASE (g_board.ram_base)
#endif

#endif /* __aarch64__ */

#endif /* XNU_LOADER_PLATFORM_H */
