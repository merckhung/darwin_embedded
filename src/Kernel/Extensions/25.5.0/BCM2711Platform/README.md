# Raspberry Pi 4B platform

`BCM2711Platform` is the platform module for this project's BCM2711/Cortex-A72
XNU configuration. It replaces the imported multi-board ARM platform expert.
It registers CPU0 with XNU, exposes GIC-400 device interrupt vectors and
publishes the firmware framebuffer nub and calendar service.

XNU initializes and owns the GICv2 hardware in `osfmk/arm64/sleh.c`; the module
uses four BCM2711-specific kernel exports for device interrupts and SGI0 IPIs.
There is no duplicate GIC configuration or mapping in the extension.
Only the Raspberry Pi 4B personality is present. Apple AIC, generic QEMU
`virt`, Allwinner, SG2002, Pi Zero/Pi 3 and PSCI code are absent.

The current device tree describes one CPU. Secondary startup remains
unsupported, and configurations advertising more than one CPU fail rather
than invoking an unavailable firmware service. Pi spin-table startup must be
implemented before enabling SMP. The optional `boot-time` argument supplies
epoch seconds; otherwise the calendar starts at zero and advances with uptime.

This implementation follows the public XNU IOCPU/IOPlatformExpert interfaces
and the existing embedded boot contract. Its design was informed by the former
PureDarwin implementation; changing names does not remove the repository's
remaining upstream attribution. Physical Pi 4B boot still needs a firmware
loading path in place of QEMU's injected images and seed, plus hardware testing.
