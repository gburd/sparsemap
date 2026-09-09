# Releasing the `sparsemap` crate

The crate is release-ready: `cargo publish --dry-run` succeeds and
`cargo package` produces `target/package/sparsemap-<version>.crate`.
Publishing needs a crates.io API token (see step 3) and push access to
the forges.

Versions are kept in lockstep with the C library and the Python
binding, so a crate release exists even when the crate itself is
functionally unchanged; `scripts/check_version_consistency.sh` verifies
all eight sources agree before you tag.  Note that not every C release
reaches crates.io -- 5.4.0 and 5.5.0 were lockstep-only bumps and the
crate went 5.1.0 straight to 5.5.1 -- which is fine, but means
`max_version` on crates.io is not a reliable read of what the C library
is at.

## 1. Pre-flight (already green in CI)

```sh
cd rust
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test                      # unit + proptest + read-direction wire fixtures
sh ci/wire_compat.sh            # write-direction: C reads Rust's bytes (needs a C toolchain)
cargo build --no-default-features   # no_std
cargo publish --dry-run
```

## 2. Tag

Tags are namespaced because the C library and the Rust crate share one
repository and version line:

- `v3.0.0`      — the C library release.
- `rust-v3.0.0` — the Rust crate release.

Both annotated tags already exist locally:

```sh
git push origin v3.0.0 rust-v3.0.0
```

## 3. Publish to crates.io

```sh
cd rust
cargo publish
```

Authentication comes from the `CARGO_REGISTRY_TOKEN` environment
variable when it is set, so no `cargo login` and no `--token` flag is
needed on a machine that already exports it.  Otherwise run
`cargo login` once and paste a crates.io API token.

Use a recent cargo: the pinned dev shell's toolchain is old enough that
a transitive dev-dependency fails to parse (`feature 'edition2024' is
required`), which looks like a problem with this crate but is not.
`nix shell nixpkgs#cargo nixpkgs#rustc` works for `cargo test`,
`cargo publish` and everything else here.

Verify afterwards that the registry agrees, rather than trusting the
upload message:

```sh
curl -s https://crates.io/api/v1/crates/sparsemap |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["crate"]["max_version"])'
```

A stronger check is to depend on the freshly published version from a
throwaway project and run one assertion against it; that catches a
package that uploaded but does not resolve or build for consumers.

`docs.rs` builds the documentation automatically after publish.  The
crate is pure Rust with no build script, so the docs.rs build needs no
C toolchain.

## 4. Release packages on GitHub and Codeberg

Attach the packaged crate as a release asset on both forges:

```sh
ART=rust/target/package/sparsemap-3.0.0.crate

# Codeberg (Forgejo) — primary
#   Web UI: Releases -> New release -> tag rust-v3.0.0 -> attach $ART
# or with the Forgejo `tea` CLI:
tea release create --repo gregburd/sparsemap --tag rust-v3.0.0 \
    --title "sparsemap (Rust) 3.0.0" --note-file rust/CHANGELOG.md \
    --asset "$ART"

# GitHub mirror
gh release create rust-v3.0.0 "$ART" \
    --title "sparsemap (Rust) 3.0.0" --notes-file rust/CHANGELOG.md
```

The C library's `v3.0.0` release is created the same way against the
`v3.0.0` tag; its source tarball is the repository at that tag (the
library is just `sm.h` + `sm.c`).

## Version bumps

Keep the C and Rust versions aligned.  Update, in lock-step:

- `meson.build` `version` and `sm.h` `SM_VERSION_*` (C);
- `rust/Cargo.toml` `version` and `rust/CHANGELOG.md` (Rust).

`scripts/check_version_consistency.sh` guards the C side in CI.
