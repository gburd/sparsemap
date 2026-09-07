# Roadmap

Where sparsemap is, what is deliberately settled, and what is actually
open.  This replaces the seven-phase cleanup plan that ran from the
pre-1.0 rewrite through v5.x; those phases are all delivered and the
plan file was retired.

## What sparsemap is now

Three implementations of one data structure, kept in version lockstep:

| Component | Branch | Language | Published as |
|-----------|--------|----------|--------------|
| Reference library | `main` | C99, two files (`sm.c` + `sm.h`) | vendored source |
| Rust port | `ports/rust` (`rust/`) | Rust, no `unsafe` | crates.io `sparsemap` |
| Python binding | `ports/rust` (`python/`) | PyO3 over the Rust crate | (not yet on PyPI) |

The C library is the reference: it defines the wire format and the
semantics the other two are tested against.  The Rust port is an
independent implementation, not a binding -- it is validated against C
by fixture exchange in both directions, not by calling into it.  The
Python binding wraps the Rust crate.

## Settled decisions

These are closed.  Reopening any of them needs new evidence, not a
preference.

- **Scalar only, no SIMD.**  See the SIMD section of `README.md` for the
  measurements.  `-O3 -march=native` already auto-vectorizes five loops
  and buys only ~6-13% on set operations, which is the same order as the
  gather overhead a hand-written kernel would have to repay first.
- **Not thread-safe, and not becoming so.**  Concurrent readers of an
  unmutated map are fine; mutation needs external synchronisation.  The
  `experiment/thread-safe` branch is a 2024 archive.
- **Version lockstep across all three implementations**, even when one
  is functionally unchanged, enforced by
  `scripts/check_version_consistency.sh` over eight sources (meson,
  `sm.h`'s three version macros, both `Cargo.toml`s, `pyproject.toml`,
  and both man pages).  A C-only fix still bumps the Rust crate.  The
  cost is occasional no-op releases; the benefit is that "sparsemap
  5.6.0" names one thing.
- **The ports deliberately lag the C accelerator APIs.**
  `sm_locator_*`, `sm_contains_many` and `sm_contains_cached` exist
  because the C representation is a flat byte stream that otherwise
  needs a linear chunk walk.  The Rust port's `BTreeMap` index already
  gives O(log n) lookups, so porting them would add surface area for no
  gain.  This is a decision, not a gap.
- **No stateful iterator object.**  `sm_next_member` / `sm_prev_member`
  plus a caller-owned `sm_cursor_t` already give allocation-free
  traversal in both directions; an opaque heap iterator would be six
  functions and a malloc duplicating a three-line loop.
- **Wire format v2 is stable.**  Any future change needs a version bump
  in the header and a reader that accepts both.

## Open work

Ordered by value, not by size.

### 1. Publish the Python binding to PyPI

The wheel builds, `ty` is clean and the test suite passes, but the
package has never been published and the `sparsemap` name on PyPI has
not been checked.  Until then the binding is source-only, which is the
one thing a Python consumer cannot easily work with.

### 2. Keep the ports current with the C fixes

The C library received seven correctness fixes in the 5.5.0 cycle (see
`CHANGELOG`).  Because the Rust port is an *independent* implementation,
each one has to be assessed against it separately -- the same bug may or
may not exist there.  Specifically worth checking: the both-RLE
`sm_difference` path, the `sm_select` word-boundary off-by-one, and the
`sm_offset` chunk-emission logic.

### 3. Raise branch coverage above ~79%

Line coverage is 91.2% and function coverage 99.3% (only the diagnostic
printer `__sm_diag_` is never called), but branch coverage sits at 78.9%.
Analysis of what remains, so this is not re-derived:

- The `goto fail` arms in the set-operation merge loops are not
  reachable by starving the allocator.  `sm_union` sizes its result at
  `a->m_data_used + b->m_data_used` and never grows it (measured: 6
  `__sm_ensure_capacity` calls, 0 growths, on a 12000-bit input).  They
  are defensive code.
- Roughly 650 of the 2829 arc records are inlined duplicates of the
  `SM_ALWAYS_INLINE` chunk primitives, which gcov counts separately per
  call site.  Rebuilding them `noinline` drops the count to 2183 and
  coverage lands at 77.8%, i.e. the duplicates are covered at the same
  rate -- so the figure is genuine, and de-inlining hot-path functions
  to improve a metric would be the wrong trade.
- The remaining honest targets are inside `__sm_separate_rle_chunk`
  (40 arcs), `__sm_chunk_rank` (19) and `sm_split` (18).

The number to manage is the count of distinct source lines with an
uncovered arc, not the percentage.

### 4. Sustain the platform matrix

Currently verified per release: x86_64 Linux (gcc + clang, plus
ASan/UBSan and valgrind), sparcv9 big-endian (OpenIndiana), FreeBSD
amd64, i686 ILP32, and aarch64 / riscv64 / s390x cross-built and run
under qemu in CI.

Big-endian coverage earns its keep: it caught a descriptor byte-walk bug
that had been shipping since v1.0.0 and that **no little-endian test can
detect**, because a byte walk and a shift are identical there.
`scripts/check_endian_safety.sh` is the static backstop.

Win11/aarch64 is verified by hand when the machine is reachable and is
not part of the automated matrix.

### 5. Deferred, with a reason

- **An aligned-alloc hook in `sm_allocator_t`** -- only needed if SIMD
  work ever happens, and adding it now would be a speculative API slot.
- **`sm_shrink_to_fit`** -- no consumer has asked; `sm_owned_copy`
  covers the same need at the cost of one copy.
