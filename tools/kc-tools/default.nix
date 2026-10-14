{ stdenv
, lib
, cmake
}:

stdenv.mkDerivation rec {
  pname = "kc-tools";
  version = "0.1";

  src = ./.;

  nativeBuildInputs = [
    cmake
  ];

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp prelink-builder $out/bin/prelink-builder
    cp kc-builder $out/bin/kc-builder

    runHook postInstall
  '';

  meta = with lib; {
    description = "Tools for Apple's XNU PrelinkedKernel and KernelCollection formats";
    platforms = platforms.linux ++ platforms.darwin;
  };
}
