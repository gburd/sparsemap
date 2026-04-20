{
  description = "A sparse bitmapped index library in C.";

  inputs = {
    # nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    nixpkgs.url = "github:NixOS/nixpkgs/23.11";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, ... }
    @inputs: inputs.utils.lib.eachSystem [
      "x86_64-linux" "i686-linux" "aarch64-linux" "x86_64-darwin"
    ] (system:
      let pkgs = import nixpkgs {
            inherit system;
            overlays = [];
            config.allowUnfree = true;
          };
      in {
        flake-utils.inputs.systems.follows = "system";
        devShell = pkgs.mkShell rec {
          name = "sparsemap";
          packages = with pkgs; [
            act
            autoconf
            clang
            cmake
            ed
            gcc
            gdb
            gettext
            graphviz-nox
            libtool
            m4
            ninja
            perl
            pkg-config
            python3
            ripgrep
            valgrind
          ];

          buildInputs = with pkgs; [
            libbacktrace
            glibc.out
            glibc.static
          ];

          shellHook = let
            icon = "f121";
          in ''
        export PS1="$(echo -e '\u${icon}') {\[$(tput sgr0)\]\[\033[38;5;228m\]\w\[$(tput sgr0)\]\[\033[38;5;15m\]} ($(git rev-parse --abbrev-ref HEAD)) \\$ \[$(tput sgr0)\]"
        '';
        };
        DOCKER_BUILDKIT = 1;
      });
}
