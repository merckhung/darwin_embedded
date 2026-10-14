{ stdenv
, lib
  # auto (the FDT decides), qemuvirt (virt Cortex-A7), rv1106 (Luckfox Pico),
  # rk3506 (Luckfox Lyra) or a20 (Allwinner A20, e.g. Banana Pi)
, platform ? "auto"
}:

assert platform == "auto" || platform == "qemuvirt" || platform == "rv1106"
  || platform == "rk3506" || platform == "a20";

# xnu arm32 boot shim as a Linux zImage: QEMU -kernel, U-Boot bootz. The
# kernel Mach-O and boot-args.txt come from a newc cpio initrd.
stdenv.mkDerivation {
  pname = "xnu-loader-arm32";
  version = "0.1";
  src = ./.;
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    flags="-march=armv7-a -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector"
    flags="$flags -nostdlib -fpie -O2 -Wall"
    ${lib.optionalString (platform != "auto") "flags=\"$flags -DXNU_LOADER_PLATFORM_${lib.toUpper platform}\""}
    $CC $flags -c entry.S -o entry.o
    $CC $flags -c boot.c -o boot.o
    $LD -nostdlib -pie --no-dynamic-linker -z notext --no-warn-rwx-segments \
      -T linker.ld -o boot32.elf entry.o boot.o "$($CC -print-libgcc-file-name)"
    if $READELF -W -r boot32.elf | awk 'NR>2 && $3 ~ /^R_ARM/ && $3 != "R_ARM_RELATIVE" && $3 != "R_ARM_NONE"' | grep -q .; then
      $READELF -W -r boot32.elf | grep -v R_ARM_RELATIVE | head -20
      echo "non-relative dynamic relocations"; exit 1
    fi
    $OBJCOPY -O binary -R .bss boot32.elf zImage
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 boot32.elf $out/boot/xnu-loader-arm32.elf
    install -Dm644 zImage $out/boot/xnu-loader-arm32.zImage
    runHook postInstall
  '';

  meta.description = "xnu arm32 boot shim as a Linux zImage";
}
