{
  description = "Sparse, compressed bitmap library (C, MIT-licensed).";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    utils.url = "github:numtide/flake-utils";

    # The Hegel property-testing server.  Pinned to v0.9.1 to match
    # the protocol the bundled hegel-c client speaks (the C submodule
    # in the hegel monorepo was protocol-synced to hegel-core v0.9).
    # Exposed to the test harness via HEGEL_SERVER_COMMAND; the client
    # spawns it over a stdin/stdout pipe, which v0.9.x auto-detects.
    hegel-core.url =
      "git+https://github.com/hegeldev/hegel-core?dir=nix&ref=refs/tags/v0.9.1";
  };

  outputs = { self, nixpkgs, hegel-core, ... } @inputs:
    inputs.utils.lib.eachSystem [
      "x86_64-linux" "i686-linux" "aarch64-linux"
      "x86_64-darwin" "aarch64-darwin"
    ] (system:
      let pkgs = import nixpkgs {
            inherit system;
            overlays = [];
            config.allowUnfree = true;
          };
          # The Hegel server binary for this system, if hegel-core
          # provides one (it covers the flakeExposed systems).
          hegelBin =
            if (hegel-core.packages ? ${system})
            then pkgs.lib.getExe hegel-core.packages.${system}.default
            else null;
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

            # Property-based testing: hegel-c is built from source by the
            # shellHook against these libraries (it has no nixpkgs
            # package and links -lcbor -lz; cmocka is used by its own
            # test suite).  cmake drives the hegel-c build.
            cmake
            libcbor
            zlib
            cmocka

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
            echo "sparsemap dev shell -- meson primary, autotools kept for legacy branches"
            echo "  build:  meson setup builddir && ninja -C builddir"
            echo "  test:   meson test -C builddir --print-errorlogs"
            ${pkgs.lib.optionalString (hegelBin != null) ''
            export HEGEL_SERVER_COMMAND=${hegelBin}

            # Build the hegel-c client library from the local checkout
            # (../hegel/c) into $PWD/.hegel-c so meson's -Dhegel probe
            # finds hegel/hegel.h + libhegel against the SAME libcbor
            # this shell provides (avoids the 0.12-vs-0.13 ABI clash
            # seen when linking a prebuilt store libhegel).
            export HEGEL_C_SRC="''${HEGEL_C_SRC:-$PWD/../hegel/c}"
            export HEGEL_C_PREFIX="$PWD/.hegel-c"
            if [ -f "$HEGEL_C_SRC/CMakeLists.txt" ] \
               && [ ! -f "$HEGEL_C_PREFIX/lib/libhegel.a" ]; then
              echo "  hegel: building hegel-c from $HEGEL_C_SRC ..."
              cmake -S "$HEGEL_C_SRC" -B "$HEGEL_C_PREFIX/_build" \
                    -DCMAKE_INSTALL_PREFIX="$HEGEL_C_PREFIX" \
                    -DCMAKE_INSTALL_LIBDIR=lib \
                    -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1 \
                && cmake --build "$HEGEL_C_PREFIX/_build" --target install \
                    >/dev/null 2>&1 \
                && echo "  hegel: hegel-c installed to $HEGEL_C_PREFIX" \
                || echo "  hegel: hegel-c build FAILED (property tests will be skipped)"
            fi
            if [ -f "$HEGEL_C_PREFIX/lib/libhegel.a" ] \
               || [ -f "$HEGEL_C_PREFIX/lib/libhegel.so" ]; then
              export CMAKE_PREFIX_PATH="$HEGEL_C_PREFIX''${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
              export PKG_CONFIG_PATH="$HEGEL_C_PREFIX/lib/pkgconfig''${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
              export CPATH="$HEGEL_C_PREFIX/include''${CPATH:+:$CPATH}"
              export LIBRARY_PATH="$HEGEL_C_PREFIX/lib''${LIBRARY_PATH:+:$LIBRARY_PATH}"
              export LD_LIBRARY_PATH="$HEGEL_C_PREFIX/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
              echo "  hegel: server=$HEGEL_SERVER_COMMAND  client=$HEGEL_C_PREFIX"
              echo "  test:   meson setup build -Dhegel=enabled && meson test -C build"
            fi
            ''}
          '';
        };
      });
}
