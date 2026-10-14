# Architecture

Both targets run XNU 12377.121.6, reporting Darwin 25.5.0, with matching published
Darwin library sources and exported kernel headers. All executable programs,
shared libraries and modules installed in either root are native Mach-O for
the target CPU. Root builders check inventories, architecture, dependencies
and code signatures before producing HFS+ images.

## Boot and kernel

x86_64 uses QEMU q35, OVMF and the `pc_uefi` loader. The loader reserves
the memory disk and loads the kernel collection. ARM64 uses QEMU raspi4b,
Cortex-A72 and a loader supplying a device tree, framebuffer and memory disk.
One ARM CPU runs; secondary CPUs remain parked. Loaders receive fresh boot
entropy; EFI requires firmware RNG or a fresh seed.

`BCM2711Platform` registers the Pi's boot CPU, device interrupts and boot
framebuffer. XNU owns the GIC-400's GICv2 distributor and CPU interface,
including timer/IPI dispatch. The module has no Apple AIC, GICv3, PSCI,
Allwinner, SG2002 or earlier-Pi path. `pc_uefi` accepts the x86_64 kernel
collection and HFS+ RAM disk from its own boot volume. It has no ARM, RISC-V,
legacy BIOS, network boot or Apple-specific boot identity paths.

The PC kernel's physical link base is 65 MiB. The UEFI loader checks this
contract and keeps the destination clear of firmware runtime, ACPI and NVS
reservations. Boot arguments/tables sit at 96 MiB, above the collection.
The loader stays in long mode and calls a tagged native 64-bit XNU entry.
A private-stack bridge delegates the collection copy to C; XNU installs its
own GDT and bootstrap page tables before entering normal kernel startup.
The loader checks the entry tag and rejects incompatible kernels.

Collection tools accept thin x86_64/ARM64 images. ARM32/Thumb, RISC-V and
standalone classic-output paths have been removed, together with inactive
CMake/Nix recipes. The shared prelink assembly library remains part of the
collection builder. PC relocations still include 32-bit fields inside
64-bit images. The PC APIC/PIC, storage, HFS+, crypto, pthread and generated
Mach/IOKit interfaces remain dependencies of the selected boot paths.

The [four-variant validation record](validation.json) binds fresh minimal and
Xfce boots and screenshots to the replacement implementations' boot inputs.
The [platform validation record](platform-validation.json) additionally describes
framebuffer ownership and input driver checks. These checks cover QEMU
raspi4b and q35/OVMF. Physical board boot and ARM SMP need separate validation;
the Pi firmware loading path still needs to replace QEMU's injected images.

Matching platform, storage, HFS and pthread extensions support the real HFS+
root. Kernel and extension segments retain checked protections. The build
identity uses a project name and relative object identifier, avoiding
workstation usernames and paths in the Darwin version string.

## Native runtime

Source-built Darwin libsyscall, platform, pthread, libc, malloc and math
components provide the application runtime. The Mach-O loader supports
rebasing, symbol binding, initialization, module loading, thread local
variables, weak symbols and image/unwind metadata needed by the desktop.
Darwin syscalls and Mach traps enter the actual XNU kernel. Native adaptations
provide APIs required by the selected application profile.

PID 1 initializes the console, launches dash, reaps children and respawns an
exited shell. Native account data, ownership and permissions are installed.
BusyBox provides 111 applets. Darwin `ps` uses `__proc_info` and
`sysctl(KERN_PROCARGS2)`; `df` uses `getfsstat` and `statfs`. Portable applets
compile against the Darwin SDK with adaptations in private build copies.
`/bin/sh` remains dash; `/bin/uname` performs real kernel sysctl queries.

## Desktop

Original Xorg uses the guest framebuffer and native input drivers. GTK 3,
GLib, Pango, Cairo, font and accessibility libraries support the original
Xfce4 session, window manager, settings daemon, panels and desktop. The
original xfce4-terminal runs VTE with a real Darwin PTY and interactive dash.
A startup launcher starts this stack automatically.

Verification injects keyboard events through QEMU's emulated devices. The
terminal's foreground PTY child reads a fresh challenge; its shell executes
a separately typed command. Readiness/completion records use temporary files
and atomic rename. ARM initializes the framebuffer at 1024×768 and 32 bits;
x86_64 uses the actual EFI GOP mode and stride. GTK uses zoom-to-fit.

## Boundaries

Optional services such as ConsoleKit and a system bus are omitted. Physical
hardware, ARM SMP, additional applications, network services and general
peripheral support require separate validation. VTE is built without GnuTLS
scrollback encryption and reports its upstream warning.

XNU checks CodeDirectory structure and code-page hashes. The embedded policy
accepts empty-entitlement ad-hoc programs while denying entitlement privileges
and claimed platform/team identity. Apple's complete AMFI/Image4 trust-cache
services are unavailable. The embedded crypto provider is scoped to this
profile; Apple's complete corecrypto and compiler-provided allocator type
segregation are unavailable.

Bazel owns the build graph. Applications compile directly using Clang and
upstream code/data generators. Host tools and loaders use their platform
formats, separate from the Mach-O guest root. Original imported build recipes
remain upstream source material; they are not application build entry points.
