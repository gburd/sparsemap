# Contributing to sparsemap

Thanks for your interest.  Sparsemap is a small library with a
narrow scope; contributions are welcome but tend to be deliberate.

## Scope

Sparsemap is a compressed bitmap library targeting embedded use
cases (PostgreSQL extensions, undo logs, posting trees, bloom
builders).  Contributions that fit:

- Bug fixes with regression tests
- Performance work backed by reproducible benchmarks
  (`tests/soak.c` is the comparison harness)
- Portability fixes (new architectures, new compilers)
- API additions that close concrete gaps in the consumer-facing
  surface
- Documentation improvements

Contributions that probably don't fit:

- Speculative features without a downstream consumer asking for
  them
- Wire-format changes (the format is stable since v1.0; changes
  go through a major version bump and have to justify the cost
  to every existing consumer)
- SIMD work without measured-bottleneck profile data (see the
  README's "Future work: SIMD" section)
- Cosmetic refactors that don't change behavior

If you're not sure, open an issue first and describe what you want
to do.  A short conversation up front saves everyone time.

## Building and testing

```
nix develop --command bash -c 'meson setup builddir && ninja -C builddir && meson test -C builddir'
```

Without Nix:

```
meson setup builddir
ninja -C builddir
meson test -C builddir
```

Required tools: meson ≥ 0.61, ninja, a C99 compiler (gcc or clang).

## Sanitizers

Every patch must pass under both ASan and UBSan:

```
meson setup builddir-asan -Db_sanitize=address
ninja -C builddir-asan
meson test -C builddir-asan

meson setup builddir-ubsan -Db_sanitize=undefined
ninja -C builddir-ubsan
meson test -C builddir-ubsan
```

CI runs both on every push.  Patches that fail either are returned.

## Coverage

```
./scripts/measure_coverage.sh builddir-coverage
```

We aim for ≥ 80% line and ≥ 80% branch coverage.  New code without
tests is unwelcome; new code without branch coverage is unwelcome.

## Fuzzing

The library has a libFuzzer harness for the deserialization path:

```
meson setup builddir-fuzz -Dfuzz=enabled
ninja -C builddir-fuzz
./builddir-fuzz/tests/fuzz_deserialize tests/fuzz-corpus/ -max_total_time=60
```

Patches that touch `sm_open`, `sm_deserialize`, or any chunk-codec
parsing path should be exercised against the fuzzer for at least
10 minutes locally before submission.

## Code style

- C99.  Use `-Wall -Wextra -Wpedantic` without warnings.
- Functions ≤ 100 lines, cyclomatic complexity ≤ 8.
- Public symbols are `sm_*` (functions) and `SM_*` (macros).
- Internal helpers are `__sm_*`.  Static where possible.
- The type `sm_t` stays — do not rename.
- 2-space indentation, no tabs in C source.
- One commit per logical change.  Imperative-mood subject line
  ≤ 72 chars.  No "WIP" or "fix typo" commits in the history of
  a submitted patch.

## Submitting

1. Fork on Codeberg.  GitHub is a mirror; PRs there are
   accepted but Codeberg is primary.
2. Create a feature branch off `main`.  Name it
   `fix/short-description` or `feat/short-description`.
3. Run the full test matrix locally (regular + ASan + UBSan +
   coverage).
4. Open a pull request against `main`.  Describe what the change
   does and why.  Reference any issue.
5. Be patient.  This is a single-maintainer project.

## Vulnerability reports

See `SECURITY.md` for the process.  Don't open public issues for
security problems.

## Copyright

The library is MIT-licensed.  By submitting a patch you affirm
that you have the right to do so under MIT.  No CLA.
