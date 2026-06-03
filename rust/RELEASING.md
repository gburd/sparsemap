# Releasing the `sparsemap` crate

The crate is release-ready: `cargo publish --dry-run` succeeds and
`cargo package` produces `target/package/sparsemap-<version>.crate`.
The steps below require credentials this environment does not hold
(a crates.io API token and push access to the forges), so they are the
maintainer's to run.

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
cargo login        # paste a crates.io API token (one time)
cd rust
cargo publish
```

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
