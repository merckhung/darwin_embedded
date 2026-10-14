{ stdenv
, lib
, gnu-efi
, embeddedInitrd ? null
  # aarch64 target machine, as in ../default.nix: bcm2837, generic, qemuvirt, sun50i or sg2002
, platform ? "qemuvirt"
  # generic boards: a 4k-page kernel, and a UART for the progress marks before the tree is read
, kernel4k ? false
, markUart ? null
}:

assert platform == "bcm2837" || platform == "generic" || platform == "qemuvirt" || platform == "sun50i" || platform == "sg2002";

# xnu-loader as an arm64 Linux Image: U-Boot `booti`, QEMU `-kernel`, or any
# loader that speaks Documentation/arch/arm64/booting.rst. Modules (the kernel
# collection, boot-args.txt) come from the initrd cpio.
stdenv.mkDerivation {
  pname = "xnu-loader-kernel-arm64";
  version = "0.1";
  src = ../.;
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p build
    common="-ffreestanding -fno-stack-protector -fno-stack-check -fpie -fshort-wchar"
    common="$common -funsigned-char -O2 -ggdb -Wno-pointer-sign"
    includes="-Iefi-emulation -Iinclude -Isrc -I${gnu-efi}/include -I${gnu-efi}/include/efi -I${gnu-efi}/include/efi/aarch64"
    includes="$includes -I${gnu-efi}/include/efi/protocol"
    defines="-DEFI_FUNCTION_WRAPPER -DCONFIG_aarch64 -DCONFIG_LOADER_aarch64 -DXNU_LOADER_PLATFORM_${lib.toUpper platform} -DLEGACY_BIOS"
    defines="$defines ${lib.optionalString kernel4k "-DXNU_LOADER_KERNEL_4K"} ${lib.optionalString (markUart != null) "-DXNU_LOADER_MARK_UART=${markUart}"}"

    objects=""
    for source in kernel/entry/linux-arm64.S efi-emulation/exceptions-arm64.S src/jump.S; do
      object="build/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $CC -c "$source" -o "$object" -fpie $defines
      objects="$objects $object"
    done
    $CC -c kernel/entry/embedded.S -o build/entry-embedded.S.o -fpie \
      ${lib.optionalString (embeddedInitrd != null) "'-DEMBED_CPIO=\"${embeddedInitrd}\"'"}
    objects="$objects build/entry-embedded.S.o"
    # Runs with the MMU off, where unaligned accesses fault
    $CC -c kernel/arm64.c -o build/kernel-arm64.c.o $common -mstrict-align $defines $includes
    $CC -c src/fdt_board.c -o build/src-fdt_board.c.o $common -mstrict-align $defines $includes
    objects="$objects build/kernel-arm64.c.o build/src-fdt_board.c.o"
    for source in kernel/cpio.c efi-emulation/exceptions.c efi-emulation/firmware.c \
      efi-emulation/modfs.c efi-emulation/storage.c \
      src/main.c src/app.c src/boot.c src/console.c src/devtree.c src/devtree-fdt.c src/fileio.c \
      src/lowmem.c src/macho.c src/serial.c; do
      object="build/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $CC -c "$source" -o "$object" $common $defines $includes
      objects="$objects $object"
    done

    libgcc="$($CC -print-libgcc-file-name)"
    $LD -nostdlib -pie --no-dynamic-linker -z notext --no-warn-rwx-segments \
      --allow-multiple-definition -z max-page-size=0x1000 \
      -T kernel/linker-arm64.ld -o build/xnu-loader.elf $objects \
      -L${gnu-efi}/lib -lefi "$libgcc"

    # Only R_AARCH64_RELATIVE is applied by the entry stub
    if $READELF -W -r build/xnu-loader.elf | awk 'NR>2 && $3 ~ /^R_AARCH64/ && $3 != "R_AARCH64_RELATIVE" && $3 != "R_AARCH64_NONE"' | grep -q .; then
      $READELF -W -r build/xnu-loader.elf | grep -v R_AARCH64_RELATIVE | head -20
      echo "non-relative dynamic relocations"; exit 1
    fi
    $OBJCOPY -O binary -R .bss build/xnu-loader.elf build/Image
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 build/xnu-loader.elf $out/boot/xnu-loader-arm64.elf
    install -Dm644 build/Image $out/boot/xnu-loader.Image
    runHook postInstall
  '';

  meta = {
    description = "xnu-loader as an arm64 Linux Image";
    license = lib.licenses.bsd3;
  };
}
