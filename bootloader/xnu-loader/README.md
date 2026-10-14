# xnu-loader

A GNU-EFI UEFI application (`BOOTX64.EFI`) that boots the XNU/Darwin kernel on
x86-64 QEMU. It reads the kernel Mach-O (or a `kc-tools` kernel collection) from
the ESP, builds the `boot_args` struct XNU's `pstart` expects, exits EFI boot
services, copies segments to their physical addresses, then jumps to the kernel
entry via a 64->32-bit mode switch.

## Building

From the repository root, run:

```sh
bazel build //bootloader/xnu-loader:efi
bazel run //:boot_x86_64
```

The first target builds GNU-EFI and the loader directly from the bundled
sources, producing `bazel-bin/bootloader/xnu-loader/BOOTX64.EFI`. Its Bazel
actions invoke the compiler, archiver, linker, and PE converter directly.
The second target builds the boot image and verifies the Darwin diagnostic
shell in QEMU.

## Legacy BIOS

The original x86 BIOS bootstrap sources remain in `legacy/`. The supported
repository Bazel boot target uses UEFI. See [legacy/README.md](legacy/README.md)
for the historical BIOS disk layout and hardware scope.

## Why?
PureDarwin.

### Why is this so hacky?
I made it to work, not to be clean.
