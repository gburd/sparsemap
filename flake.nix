{
  description = "A Concurrent Skip List library for key/value pairs.";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs =
    { self
    , nixpkgs
    , flake-utils
    , ...
    }:
    flake-utils.lib.eachDefaultSystem (system:
      let
#        pkgs = nixpkgs.legacyPackages.${system};
        pkgs = import nixpkgs {
          inherit system;
          config = { allowUnfree = true; };
        };
      in
      {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            autoconf
            bashInteractive
            clang-tools
            ed
            gdb
            graphviz-nox
            meson
            python311Packages.rbtools
          ];
        };
        buildInputs = with pkgs; [
          glibc
        ];
        nativeBuildInputs = with pkgs.buildPackages; [
          act
          binutils
          coreutils
          gcc
          gettext
          libtool
          m4
          make
          perl
          pkg-config
          ripgrep
        ];
      });
}
