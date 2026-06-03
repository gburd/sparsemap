{
  description = "Sparse, compressed bitmap library (C, MIT-licensed).";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, ... } @inputs:
    inputs.utils.lib.eachSystem [
      "x86_64-linux" "i686-linux" "aarch64-linux"
      "x86_64-darwin" "aarch64-darwin"
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
            # Build system: meson primary, autotools kept transitionally
            # for branches that still use it.
            meson
            ninja
            pkg-config

            # Compilers and friends.
            clang
            gcc
            gdb

            # Documentation toolchain.
            doxygen
            graphviz-nox

            # Quality / sanitizer tooling.
            valgrind
            cppcheck

            # Coverage.
            lcov

            # Misc dev environment.
            ed
            perl
            ripgrep
            (python3.withPackages (ps: [ ps.matplotlib ps.numpy ]))

            # Legacy autotools (kept for archive branches and one-off
            # comparisons; main uses meson).
            autoconf
            automake
            libtool
            m4
          ];

          shellHook = ''
            echo "sparsemap dev shell — meson primary, autotools kept for legacy branches"
            echo "  build:  meson setup builddir && ninja -C builddir"
            echo "  test:   meson test -C builddir --print-errorlogs"
          '';
        };
      });
}
