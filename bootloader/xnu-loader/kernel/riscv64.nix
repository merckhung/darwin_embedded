{ stdenvNoCC
, lib
, llvmPackages
, gnu-efi
  # compiler-rt built for riscv64 (lp64d), only its builtins archive is linked
, compilerRt
, embeddedInitrd ? null
}:

# xnu-loader as a risc-v linux Image: opensbi's fw_dynamic/fw_jump, u-boot booti,
# qemu -kernel. The kernel collection and boot-args.txt come from the initrd cpio.
# Built by clang and lld for a bare riscv64 target, with gnu-efi compiled from source
# because nixpkgs ships no riscv64 gnu-efi that clang can link.
let
  clang = llvmPackages.clang-unwrapped;
  lld = llvmPackages.lld;
  bintools = llvmPackages.bintools-unwrapped;
  efi = gnu-efi.src;
in
stdenvNoCC.mkDerivation {
  pname = "xnu-loader-kernel-riscv64";
  version = "0.1";
  src = ../.;
  dontConfigure = true;
  dontFixup = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p build/efi build/include build/shim
    cc="${clang}/bin/clang --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d"
    cc="$cc -mcmodel=medany -mno-relax"
    common="-ffreestanding -fno-stack-protector -fno-stack-check -fpie -fshort-wchar"
    common="$common -funsigned-char -O2 -g -Wno-pointer-sign"
    # <efi/...> spellings and the two libc headers the sources name but never use
    ln -s ${efi}/inc build/include/efi
    touch build/shim/string.h build/shim/stdlib.h
    efiinc="-I${efi}/inc -I${efi}/inc/riscv64 -I${efi}/inc/protocol"
    includes="-Iefi-emulation -Iinclude -Isrc -Ibuild/include -Ibuild/shim $efiinc"
    defines="-DEFI_FUNCTION_WRAPPER -DCONFIG_riscv64 -DCONFIG_LOADER_riscv64 -DLEGACY_BIOS"

    # gnu-efi's library, BSD licensed, from its own sources
    for source in ${efi}/lib/*.c ${efi}/lib/runtime/*.c ${efi}/lib/riscv64/*.c ${efi}/lib/riscv64/*.S; do
      object="build/efi/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $cc -c "$source" -o "$object" $common $efiinc -I${efi}/lib
    done
    ${bintools}/bin/llvm-ar rcs build/libefi.a build/efi/*.o

    objects=""
    for source in kernel/entry/linux-riscv64.S efi-emulation/exceptions-riscv64.S src/jump-riscv64.S; do
      object="build/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $cc -c "$source" -o "$object" -fpie $defines
      objects="$objects $object"
    done
    $cc -c kernel/entry/embedded.S -o build/entry-embedded.S.o -fpie \
      ${lib.optionalString (embeddedInitrd != null) "'-DEMBED_CPIO=\"${embeddedInitrd}\"'"}
    objects="$objects build/entry-embedded.S.o"
    for source in kernel/riscv64.c kernel/cpio.c efi-emulation/exceptions.c efi-emulation/firmware.c \
      efi-emulation/modfs.c efi-emulation/storage.c \
      src/riscv64.c src/app.c src/boot.c src/console.c src/devtree.c src/devtree-fdt.c \
      src/fileio.c src/lowmem.c src/macho.c src/serial.c; do
      object="build/$(basename "$(dirname "$source")")-$(basename "$source").o"
      $cc -c "$source" -o "$object" $common $defines $includes
      objects="$objects $object"
    done

    ${lld}/bin/ld.lld -nostdlib -pie --no-dynamic-linker -z notext \
      --allow-multiple-definition -z max-page-size=0x1000 \
      -T kernel/linker-riscv64.ld -o build/xnu-loader.elf $objects \
      build/libefi.a ${compilerRt}/lib/linux/libclang_rt.builtins-riscv64.a

    # only R_RISCV_RELATIVE is applied by the entry stub
    if ${bintools}/bin/llvm-readelf -W -r build/xnu-loader.elf | awk 'NR>2 && $3 ~ /^R_RISCV/ && $3 != "R_RISCV_RELATIVE" && $3 != "R_RISCV_NONE"' | grep -q .; then
      ${bintools}/bin/llvm-readelf -W -r build/xnu-loader.elf | grep -v R_RISCV_RELATIVE | head -20
      echo "non-relative dynamic relocations"; exit 1
    fi
    ${bintools}/bin/llvm-objcopy -O binary -R .bss build/xnu-loader.elf build/Image
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 build/xnu-loader.elf $out/boot/xnu-loader-riscv64.elf
    install -Dm644 build/Image $out/boot/xnu-loader.Image
    runHook postInstall
  '';

  meta = {
    description = "xnu-loader as a riscv64 Linux Image";
    license = lib.licenses.bsd3;
  };
}
