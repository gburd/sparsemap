{
  description = "Sparse, compressed bitmap library (C, MIT-licensed).";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    # A newer channel for tools absent from 24.05: `ty` (Astral's Python
    # type checker, used by the python/ binding's checks) and a current
    # maturin/uv.  Kept separate so the stable base is unchanged.
    nixpkgs-unstable.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, nixpkgs-unstable, ... } @inputs:
    inputs.utils.lib.eachSystem [
      "x86_64-linux" "i686-linux" "aarch64-linux"
      "x86_64-darwin" "aarch64-darwin"
    ] (system:
      let pkgs = import nixpkgs {
            inherit system;
            overlays = [];
            config.allowUnfree = true;
          };
          # Newer channel for tools absent from 24.05 (ty, current
          # maturin/uv) used by the python/ binding.
          pkgsUnstable = import nixpkgs-unstable {
            inherit system;
            config.allowUnfree = true;
          };
          # Official Hegel C library (hegeldev/hegel-rust/hegel-c): an
          # in-process FFI -- a prebuilt libhegel shared object plus the
          # tagged <hegel.h>.  No server, no libcbor/zlib (this replaces
          # the deprecated gburd/hegel-c socket client).  Pinned to the
          # v0.30.3 release; the property tests link -lhegel against it.
          hegelVersion = "v0.30.3";
          hegelAsset = {
            "x86_64-linux"   = { f = "libhegel-linux-amd64.so";    h = "sha256-sN6OXYTMhN8QjBMfLo6Mzq8zkMIB0zr/ExKCifxuHgI="; };
            "aarch64-linux"  = { f = "libhegel-linux-arm64.so";    h = null; };
            "x86_64-darwin"  = { f = "libhegel-darwin-arm64.dylib"; h = null; };
            "aarch64-darwin" = { f = "libhegel-darwin-arm64.dylib"; h = null; };
          };
          hegelLib =
            if (hegelAsset ? ${system}) && (hegelAsset.${system}.h != null)
            then
              let a = hegelAsset.${system};
                  soName = if pkgs.stdenv.isDarwin then "libhegel.dylib" else "libhegel.so";
                  header = pkgs.fetchurl {
                    url = "https://raw.githubusercontent.com/hegeldev/hegel-rust/${hegelVersion}/hegel-c/include/hegel.h";
                    hash = "sha256-zmldwtgH+yF9Ung1HRMaffb3pv+GCLfUJy9m6Xd0sno=";
                  };
                  so = pkgs.fetchurl {
                    url = "https://github.com/hegeldev/hegel-rust/releases/download/${hegelVersion}/${a.f}";
                    hash = a.h;
                  };
              in pkgs.runCommand "libhegel-${hegelVersion}" { } ''
                mkdir -p $out/include $out/lib
                cp ${header} $out/include/hegel.h
                cp ${so} $out/lib/${soName}
              ''
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

            # Rust port (ports/rust) + Python binding (python/).  The
            # binding is a PyO3 wheel over the Rust crate, built by
            # maturin under uv.  `ty` (Astral's type checker) gates the
            # python/ checks and is not in 24.05, so it and a current
            # maturin/uv come from nixpkgs-unstable; cargo/rustc from 24.05.
            cargo
            rustc
            pkgsUnstable.uv
            pkgsUnstable.maturin
            pkgsUnstable.ty

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
            ${pkgs.lib.optionalString (hegelLib != null) ''
            # Official hegeldev libhegel (in-process FFI).  Point the
            # compiler and runtime linker at the pinned derivation so
            # meson's -Dhegel probe finds <hegel.h> and -lhegel.
            export CPATH="${hegelLib}/include''${CPATH:+:$CPATH}"
            export LIBRARY_PATH="${hegelLib}/lib''${LIBRARY_PATH:+:$LIBRARY_PATH}"
            export LD_LIBRARY_PATH="${hegelLib}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            echo "  hegel: official libhegel ${hegelVersion} at ${hegelLib}"
            echo "  test:   meson setup build -Dhegel=enabled && meson test -C build"
            ''}
          '';
        };
      });
}
