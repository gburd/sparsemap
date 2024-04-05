{
  description = "A Concurrent Skip List library for key/value pairs.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { self
    , nixpkgs
    , flake-utils
    , ...
    }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config = { allowUnfree = true; };
        };
        supportedSystems = [ "x86_64-linux" ];
        forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
        nixpkgsFor = forAllSystems (system: import nixpkgs {
          inherit system;
          overlays = [ self.overlay ];
        });
      in {
        pkgs = import nixpkgs {
          inherit system;
          devShell = nixpkgs.legacyPackages.${system} {
            pkgs.mkShell = {
              nativeBuildInputs = with pkgs.buildPackages; [
                act
                autoconf
                clang
                ed
                gcc
                gdb
                gettext
                graphviz-nox
                libtool
                m4
                perl
                pkg-config
                python3
                ripgrep
              ];
              buildInputs = with pkgs; [
                libbacktrace
                glibc.out
                glibc.static
              ];
            };
            DOCKER_BUILDKIT = 1;
          };
        };
      });
}
