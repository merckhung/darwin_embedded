{ stdenv
, lib
, nasm
, binutils
, gnu-efi
, dosfstools
, mtools
, coreutils
, kernel ? null
, bootArgs ? null
, stage2Lba ? 1
}:

stdenv.mkDerivation {
  pname = "xnu-loader-legacy-boot";
  version = "0.1";
  src = ../.;

  nativeBuildInputs = [ nasm binutils dosfstools mtools coreutils ];
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p build

    $CC -c legacy/entry.S -o build/entry.o -m64 -ffreestanding -fno-stack-protector \
      -fno-pic -mno-red-zone
    shim_objects=""
    $CC -c efi-emulation/exceptions.S -o build/efiemu-exceptions.S.o -m64 -ffreestanding -fno-pic
    shim_objects="build/efiemu-exceptions.S.o"
    for source in legacy/main.c efi-emulation/exceptions.c efi-emulation/firmware.c \
      efi-emulation/modfs.c efi-emulation/storage.c; do
      object="build/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $CC -c "$source" -o "$object" -m64 -ffreestanding \
        -fno-stack-protector -fno-pic -mno-red-zone -mgeneral-regs-only -fshort-wchar -DGNU_EFI_USE_MS_ABI \
        -Ilegacy -Iefi-emulation -Iinclude -I${gnu-efi}/include -I${gnu-efi}/include/efi \
        -I${gnu-efi}/include/efi/x86_64 -I${gnu-efi}/include/efi/protocol
      shim_objects="$shim_objects $object"
    done
    loader_objects=""
    for source in src/main.c src/app.c src/boot.c src/console.c src/devtree.c \
      src/fileio.c src/jump.S src/lowmem.c src/macho.c src/serial.c; do
      object="build/$(basename "$source").o"
      $CC -c "$source" -o "$object" -m64 -ffreestanding -fno-stack-protector \
        -fno-pic -mno-red-zone -mgeneral-regs-only -maccumulate-outgoing-args -mno-avx \
        -fshort-wchar -funsigned-char -DGNU_EFI_USE_MS_ABI -DLEGACY_BIOS -DCONFIG_x86_64 \
        -Iinclude -Iefi-emulation -I${gnu-efi}/include -I${gnu-efi}/include/efi -I${gnu-efi}/include/efi/x86_64 \
        -I${gnu-efi}/include/efi/protocol
      loader_objects="$loader_objects $object"
    done
    libgcc="$($CC -m64 -print-libgcc-file-name)"
    $LD -nostdlib --allow-multiple-definition -T legacy/linker.ld \
      -o build/payload.elf \
      build/entry.o $shim_objects $loader_objects \
      -L${gnu-efi}/lib -lgnuefi -lefi "$libgcc"
    objcopy -O binary build/payload.elf build/payload.bin
    payload_sectors=$((($(stat -c %s build/payload.bin) + 511) / 512))

    # Stage-two size is independent of these values, so one sizing pass gives
    # us its final LBA and the constants used by the final assembly.
    nasm -f bin -DSTAGE2_LBA=${toString stage2Lba} \
      -DSTAGE2_SECTORS=1 -DPAYLOAD_SECTORS="$payload_sectors" \
      legacy/stage2.asm -o build/stage2.bin
    stage2_sectors=$((($(stat -c %s build/stage2.bin) + 511) / 512))
    test "$stage2_sectors" -gt 0
    test "$stage2_sectors" -le 64
    nasm -f bin -DSTAGE2_LBA=${toString stage2Lba} \
      -DSTAGE2_SECTORS="$stage2_sectors" \
      -DPAYLOAD_SECTORS="$payload_sectors" legacy/stage2.asm -o build/stage2.bin
    nasm -f bin -DSTAGE2_LBA=${toString stage2Lba} \
      -DSTAGE2_SECTORS="$stage2_sectors" legacy/stage1.asm -o build/stage1.bin
    test "$(stat -c %s build/stage1.bin)" -eq 512
    cp build/stage1.bin build/legacy-boot.img
    dd if=build/stage2.bin of=build/legacy-boot.img bs=512 seek=1 conv=notrunc
    dd if=build/payload.bin of=build/legacy-boot.img bs=512 \
      seek=$((${toString stage2Lba} + stage2_sectors)) conv=notrunc
    truncate -s 128M build/legacy-boot.img
    mkfs.vfat -F 32 --offset=2048 -n PUREDARWIN build/legacy-boot.img
    mmd -i build/legacy-boot.img@@1M ::EFI ::EFI/BOOT
    ${lib.optionalString (kernel != null) ''
      mcopy -i build/legacy-boot.img@@1M ${kernel} ::EFI/BOOT/KERNEL
    ''}
    ${lib.optionalString (bootArgs != null) ''
      cp ${bootArgs} build/BOOTARGS.TXT
      mcopy -i build/legacy-boot.img@@1M build/BOOTARGS.TXT ::EFI/BOOT/BOOTARGS.TXT
    ''}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/usr/standalone/i386" "$out/share/doc/xnu-loader"
    cp build/stage1.bin build/stage2.bin build/payload.bin \
      build/payload.elf build/legacy-boot.img \
      "$out/usr/standalone/i386/"
    cp legacy/README.md "$out/share/doc/xnu-loader/legacy.md"
    runHook postInstall
  '';

  meta = {
    description = "BIOS bootstrap stages for the xnu-loader legacy boot port";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.x86_64;
  };
}
