{}:
let
    nixpkgs = import <nixpkgs> {};
    stdenv = nixpkgs.stdenv;
in
stdenv.mkDerivation rec {
    name = "loggeds-${version}";
    version = "master";
    src = ./.;
    buildInputs = [
        nixpkgs.libxml2
        nixpkgs.pcre
        nixpkgs.fuse
        nixpkgs.curl.dev
        nixpkgs.sqlite.dev
    ];
    installPhase = ''
    mkdir -p $out/share/man/man1
    mkdir -p $out/bin
    mkdir -p $out/etc
    make install PREFIX=$out
    '';
}
