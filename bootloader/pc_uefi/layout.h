// SPDX-License-Identifier: BSD-2-Clause
// This physical link base must match KERNEL_BASE_OFFSET in build_xnu.py.
// Leave low firmware/ACPI NVS pages intact, including OVMF's 8 MiB range.
#define PC_KERNEL_BASE 0x04100000
#define PC_BOOT_BASE 0x06000000
#define PC_RUNTIME_BASE 0x06200000
