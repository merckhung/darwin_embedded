{ stdenv
, lib
, cmake
, coreutils
, dosfstools
, gnu-efi
, mtools
, arch ? "x86_64"
, loaderArch ? arch
  # aarch64 target machine: bcm2837, qemuvirt, sun50i sg2002, or sc8280xp. Ignored for x86_64.
, platform ? "bcm2837"
}:

assert arch == "x86_64" || arch == "aarch64";
assert platform == "bcm2837" || platform == "generic" || platform == "qemuvirt" || platform == "sun50i" || platform == "sg2002" || platform == "sc8280xp";
assert loaderArch == "x86_64" || loaderArch == "aarch64" || loaderArch == "ia32";
# ia32 firmware is only ever paired with an x86_64 kernel.
assert loaderArch != "ia32" || arch == "x86_64";

let
  # UEFI's spec-mandated removable-media fallback path name differs per
  # arch (BOOTX64.EFI, BOOTAA64.EFI, ...) - real firmware only looks for
  # its own arch's name here, and it is the *firmware's* arch that decides.
  bootFileName =
    if loaderArch == "x86_64" then "BOOTX64.EFI"
    else if loaderArch == "ia32" then "BOOTIA32.EFI"
    else "BOOTAA64.EFI";
in
stdenv.mkDerivation rec {
  pname = "xnu-loader";
  version = "0.1";

  src = ./.;

  nativeBuildInputs = [
    cmake
    coreutils
    dosfstools
    gnu-efi
    mtools
  ];

  cmakeFlags = [
    "-DGNU_EFI_DIR=${gnu-efi}"
    "-DARCH=${arch}"
    "-DLOADER_ARCH=${loaderArch}"
  ] ++ lib.optional (arch == "aarch64") "-DXNU_LOADER_PLATFORM=${platform}";

  # Cross binutils installs only target-prefixed tools in bin/, so CMake's own
  # search settles on ${binutils}/bin/<tool>, which does not exist, and the
  # build dies with "No such file or directory". Every binutils tool this
  # CMakeLists drives has to be named explicitly - the linker and objcopy
  # today. $LD/$OBJCOPY are the names the wrapper provides, which is how
  # nixpkgs' own cmake hook handles AR, RANLIB and STRIP, and it works native
  # and cross without naming store paths.
  preConfigure = ''
    cmakeFlagsArray+=("-DCMAKE_LINKER=$(command -v $LD)")
    cmakeFlagsArray+=("-DCMAKE_OBJCOPY=$(command -v $OBJCOPY)")
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out
    cp xnu-loader.efi $out/xnu-loader.efi

    mkdir -p $out/img/EFI/BOOT
    cp xnu-loader.efi $out/img/EFI/BOOT/${bootFileName}

    dd if=/dev/zero of=$out/xnu-loader.img bs=1M count=64
    mkfs.vfat -F 32 $out/xnu-loader.img
    mmd -i $out/xnu-loader.img ::EFI
    mmd -i $out/xnu-loader.img ::EFI/BOOT
    mcopy -i $out/xnu-loader.img $out/img/EFI/BOOT/${bootFileName} ::EFI/BOOT/${bootFileName}

    runHook postInstall
  '';

  meta = with lib; {
    description = "PureDarwin's XNU bootloader";
    platforms = platforms.unix;
  };
}
