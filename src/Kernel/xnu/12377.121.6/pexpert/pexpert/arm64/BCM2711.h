/* Raspberry Pi 4B / QEMU raspi4b: Cortex-A72, PL011, and GIC-400. */
#ifndef _PEXPERT_ARM_BCM2711_H
#define _PEXPERT_ARM_BCM2711_H

#define NO_MONITOR 1
#define NO_ECORE 1
#define BCM2711 1
#define ARM_ARCH_TIMER 1
#define __ARM_ARCH__ 8
#define __ARM_VMSA__ 8
#define __ARM_VFP__ 4
#define __ARM_COHERENT_CACHE__ 1
#define __ARM_DEBUG__ 7
#if !ARM_LARGE_MEMORY
#define __ARM64_PMAP_SUBPAGE_L1__ 1
#endif

#ifndef ASSEMBLER
#define PL011_UART 1
#define BCM2711_UART_BASE_PHYS 0xfe201000ULL
#define BCM2711_GICD_BASE_PHYS 0xff841000ULL
#define BCM2711_GICC_BASE_PHYS 0xff842000ULL
#endif
#endif
