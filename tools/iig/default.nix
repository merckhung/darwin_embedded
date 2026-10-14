{ stdenv
, lib
, cmake
}:

stdenv.mkDerivation rec {
  pname = "iig-tools";
  version = "0.1";

  src = ./.;

  nativeBuildInputs = [
    cmake
  ];

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp iig $out/bin/iig

    runHook postInstall
  '';

  meta = with lib; {
    description = "IOKit interface generator (iig) for DriverKit .iig files, kernel-side subset";
    platforms = platforms.linux ++ platforms.darwin;
  };
}
