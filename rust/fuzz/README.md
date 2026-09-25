# Fuzzing `SparseMap::from_bytes`

`from_bytes` documents that arbitrary bytes are safe to feed in: it must
never panic and never return a map that iterates out of order or fails
to round-trip. This [`cargo-fuzz`](https://github.com/rust-fuzz/cargo-fuzz)
target enforces that.

The `from_bytes` target decodes the input and, on a successful decode,
asserts the map iterates strictly ascending and re-serializes to a
buffer that decodes back to the same map, then exercises the
mutate/clone and set-operation paths. The fuzz profile sets
`overflow-checks = true`, so any unchecked add/mul on a chunk span
surfaces as a panic rather than a silent wrap-around.

## Run

```sh
cargo install cargo-fuzz          # once
cargo +nightly fuzz run from_bytes
```

Reproduce a saved crash:

```sh
cargo +nightly fuzz run from_bytes fuzz/artifacts/from_bytes/crash-<hash>
```

`target/`, `corpus/`, and `artifacts/` are generated and git-ignored;
do not commit them.
