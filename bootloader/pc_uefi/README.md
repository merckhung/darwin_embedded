# Embedded PC UEFI loader

`bazel build //bootloader/pc_uefi:efi` produces `BOOTX64.EFI` for x86_64
UEFI firmware. The normal minimal and desktop image targets include it.

The implementation loads an x86_64 `MH_FILESET` collection from
`EFI/BOOT/kernel` and a 64 or 128 MiB HFS+ RAM disk from `ramdisk.img` on
the EFI application's own filesystem. It validates load-command, segment and
entry-point bounds, zeroes BSS/gaps, reserves RAM-disk memory and constructs
XNU's device tree and boot arguments. The public XNU `boot.h` is included
directly so the handoff layout follows the pinned kernel source.

EFI RNG supplies boot entropy, with a 256-byte `EFI/BOOT/entropy.bin` as the
fallback. QEMU launchers replace that file with a fresh host seed on each run;
physical deployments must also provide a fresh seed when firmware RNG is absent.
No counter-based fallback is used. Optional `EFI/BOOT/boot-args.txt` supplies
ASCII arguments; the loader adds `rd=md0 rootfstype=hfs` and rejects conflicting
`rd=` arguments. GOP supplies the framebuffer's real pixel format and stride.

The memory map, EFI tables and flattened device tree occupy a reserved block
at 96 MiB. The PC kernel is linked at a physical base of 65 MiB to leave low
firmware and ACPI NVS reservations intact. The exact destination is claimed
with the firmware page allocator before boot-service exit. After ExitBootServices, the loader converts
runtime addresses, coalesces the memory map and moves to a private stack.
A C installer copies the staged collection to its link address and calls
XNU's native 64-bit entry with `boot_args` in SysV RDI.

`enter64.S` only selects the stack and calls the C installer. Paging and long
mode remain enabled. XNU's modified `osfmk/x86_64/start.s` installs its own
GDT, rebases its bootstrap page tables, changes CR3, clears firmware segment
bases and joins normal `vstart` initialization. The existing Apple 32-bit
secondary-CPU entry remains available inside XNU.

The kernel entry begins with a branch over the `DE64BOOT` version-1 tag.
The image parser requires the complete, file-backed tag before firmware exit;
a kernel with a different entry contract is rejected. Firmware using
five-level paging is also rejected because this contract uses XNU's existing
four-level tables. Invalid images, unavailable entropy and failed firmware
handoffs stop booting.

The loader, parser and stack bridge are project code under BSD-2-Clause.
`UPSTREAM.json` documents the current interface; included XNU headers and
changes to XNU retain their original Apple notices. GNU-EFI supplies the
standard EFI runtime and linking support. `compile_efi.py` builds one C
translation unit and the stack bridge with Clang, then validates the PE entry
and section bounds. Only the PC UEFI boot path is built.

Run `bazel test //bootloader/pc_uefi:image_test` for malformed images,
entry-contract rejection and BSS/gap coverage. Native guest boot and
framebuffer checks are required in addition to compilation. Physical PC boot
remains hardware-dependent and requires validation on the intended firmware
and CPU.
