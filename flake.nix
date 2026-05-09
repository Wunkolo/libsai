{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };
  outputs = {
    nixpkgs,
    flake-utils,
    ...
  }:
  flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = nixpkgs.legacyPackages.${system};
      libsai = pkgs.stdenv.mkDerivation {
        name = "libsai";
        src = pkgs.lib.sourceByRegex ./. [
          "^include.*"
          "^source.*"
          "^samples.*"
          "CMakeLists.txt"
        ];
        nativeBuildInputs = [ pkgs.cmake ];
        installPhase = ''
          mkdir -p $out/bin
          cp Thumbnail-Sai1 $out/bin/thumbnail-sai1
          cp Thumbnail-Sai2 $out/bin/thumbnail-sai2
        '';
      };
    in {
      packages.default = libsai;
    }
  );
}