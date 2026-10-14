# Legacy BIOS loader

The legacy target keeps the EFI loader as the single XNU-loading
implementation. The BIOS stages load an x86-64 payload, enter long mode, and
install the subset of EFI services used by `src/` before calling `efi_main()`.

The generated 128 MiB raw disk contains:

- an EDD stage-one boot sector;
- stage two and the flat x86-64 loader payload before LBA 2048; and
- a FAT32 boot volume at LBA 2048.

Build the stages and an empty boot volume with:

```console
nix build .#legacy-boot
```

Consumers should override `kernel` with an x86-64 XNU Mach-O. An optional
`bootArgs` file is installed under the stable `BOOTARGS.TXT` short alias used
by the read-only FAT implementation.

The initial hardware backend targets SeaBIOS and legacy-compatible primary
IDE disks. It supports E820 memory, ACPI RSDP and SMBIOS discovery, FAT32 file
reads, EFI allocation/memory-map services, serial output, and a 1024x768x32
VBE linear framebuffer exported to the shared loader as GOP. AHCI, NVMe, PXE,
and writable EFI variables are not implemented.
