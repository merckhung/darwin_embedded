{ stdenv
, lib
, binutils
, gnu-efi
, nasm
, embeddedInitrd ? null
}:

stdenv.mkDerivation {
  pname = "xnu-loader-kernel";
  version = "0.1";
  src = ../.;

  nativeBuildInputs = [ binutils nasm ];
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p build

    common="-m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone"
    common="$common -mgeneral-regs-only -fshort-wchar -DGNU_EFI_USE_MS_ABI"
    includes="-Iefi-emulation -Iinclude -I${gnu-efi}/include -I${gnu-efi}/include/efi"
    includes="$includes -I${gnu-efi}/include/efi/x86_64 -I${gnu-efi}/include/efi/protocol"

    objects=""
    for source in kernel/entry/multiboot2.S kernel/entry/linux.S efi-emulation/exceptions.S; do
      object="build/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $CC -c "$source" -o "$object" -m64 -ffreestanding -fno-pic
      objects="$objects $object"
    done
    $CC -c kernel/entry/embedded.S -o build/entry-embedded.S.o -m64 -ffreestanding -fno-pic \
      ${lib.optionalString (embeddedInitrd != null) "'-DEMBED_CPIO=\"${embeddedInitrd}\"'"}
    objects="$objects build/entry-embedded.S.o"
    for source in kernel/boot.c kernel/multiboot2.c kernel/linux.c kernel/cpio.c efi-emulation/exceptions.c \
      efi-emulation/firmware.c efi-emulation/modfs.c efi-emulation/storage.c; do
      object="build/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $CC -c "$source" -o "$object" $common $includes
      objects="$objects $object"
    done
    for source in src/main.c src/app.c src/boot.c src/console.c src/devtree.c \
      src/fileio.c src/jump.S src/lowmem.c src/macho.c src/serial.c; do
      object="build/src-$(basename "$source").o"
      $CC -c "$source" -o "$object" $common -maccumulate-outgoing-args -mno-avx \
        -funsigned-char -DLEGACY_BIOS -DCONFIG_x86_64 $includes
      objects="$objects $object"
    done

    libgcc="$($CC -m64 -print-libgcc-file-name)"
    $LD -nostdlib --allow-multiple-definition -z max-page-size=0x1000 \
      -T kernel/linker.ld -o build/xnu-loader.elf $objects \
      -L${gnu-efi}/lib -lgnuefi -lefi "$libgcc"

    # bzImage: setup sectors, then the flat payload from __kernel_start.
    objcopy -O binary -R .bss build/xnu-loader.elf build/payload.bin
    sym() { nm build/xnu-loader.elf | awk -v s="$1" '$3 == s { print "0x" $1 }'; }
    payload_size=$(stat -c %s build/payload.bin)
    file_end=$(( $(sym __file_end) - $(sym __kernel_start) ))
    [ "$payload_size" -eq "$file_end" ] || { echo "payload size mismatch"; exit 1; }
    init_size=$(( ($(sym __kernel_end) - $(sym __kernel_start) + 0xfff) & ~0xfff ))
    nasm -f bin -DSYSSIZE=$(( (payload_size + 15) / 16 )) \
      -DINIT_SIZE=$init_size kernel/entry/linux-setup.asm -o build/setup.bin
    cat build/setup.bin build/payload.bin > build/xnu-loader.bzImage
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 build/xnu-loader.elf $out/boot/xnu-loader.elf
    install -Dm644 build/xnu-loader.bzImage $out/boot/xnu-loader.bzImage
    runHook postInstall
  '';

  meta = {
    description = "xnu-loader as a Multiboot2 ELF kernel";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.x86_64;
  };
}
