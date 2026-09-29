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

- **Compile-time SIMD for the set-op kernels.**  `sm_union` /
  `sm_intersection` / `sm_difference` use hand-written AVX2/SSE2
  intrinsics for the 32-word chunk loops, selected by `__AVX2__` /
  `__SSE2__` at compile time with a scalar `#else` fallback (so ARM /
  RISC-V / s390x / SPARC still build unchanged).  No runtime dispatch,
  no wire-format change.  A wire-format SIMD extension (aligned
  contiguous-bitvec payloads) was analysed and is **not planned** — see
  the SIMD section of `README.md`.
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

Ordered by value, not by size.  Section 0 is the 5.6.0 release scope
and blocks everything below it.

### 0. Security hardening for untrusted input (5.6.0)

From the 2026-09-24 production-readiness review.  Every item below was
reproduced, not inferred; reproducers live in the review notes and each
task must land a regression test that fails without the fix.

The governing decision: **any byte stream the library did not produce
itself is untrusted**, whichever entry point it arrives through --
`sm_open`, `sm_open_copy`, `sm_deserialize`, or the Rust/Python
`from_bytes`.  A decoded map must be structurally valid or rejected,
and every operation on a valid map must terminate without touching
memory outside its buffers.

**S1. One definition of a valid map.**  `sm_validate` checks only
bounds and strictly ascending chunk starts.  It must also reject: an
RLE chunk whose length exceeds its capacity; a chunk start not aligned
to `SM_CHUNK_MAX_CAPACITY`; `start + capacity` overflowing
`uint64_t`; chunks whose spans overlap; and a chunk count that does not
match the walk.  `sm_open` and `sm_open_copy` must run it and leave the
map empty (or return NULL) on failure, exactly as `sm_deserialize`
already does.  *Accept:* every hostile case in the review (descending,
duplicate, unaligned, near-2^64, RLE len > cap, count 2^32-1) is
rejected by all three entry points.

**S2. Mutating fuzz harness in CI.**  `tests/fuzz_deserialize.c` only
reads the decoded map, so it cannot reach the write paths where the
review's crashes live.  Add the mutation battery (add/remove,
add_range/remove_range, all four set operations, offset both
directions, split) and run it in CI alongside the read-only pass.
*Accept:* the harness finds the known crashes on 5.5.1 and none on the
fixed tree in a 60 s CI smoke run plus a 30 min local campaign.

**S3. Memory safety on valid maps.**  With S1 in place, fix the
remaining crash sites the mutating harness finds on maps that *pass*
validation -- 8 of the 27 review crashes did.  Known sites: source-side
over-read in `sm_split` (`__sm_append_data` reading past the source
chunk walk), `__sm_chunk_is_rle`, `__sm_load_idx`, and a shift by 3520
at `sm.c:2954`.

**S4. Termination and amplification.**  A 24-byte input legitimately
declares a 2^31-bit run, and several operations are O(cardinality)
rather than O(chunks): `sm_xor`, `sm_hash`, the `*_cardinality`
family, `sm_jaccard_index`, `sm_extract_range`, and `sm_split`'s
per-bit loop (which does not terminate near 2^64).  Rewrite those
loops to work run-by-run and word-by-word so cost follows the encoded
size.  `sm_to_array` is inherently O(cardinality) and stays that way,
but its documentation must say so.  *Accept:* the 44-byte `sm_xor`
hang and a 2^31-bit run complete each operation in under 10 ms.

**S5. NULL-map contract.**  19 of 69 public functions segfault on a
NULL map while the rest tolerate it.  Pick one rule -- NULL is an
empty, read-only map; mutators on NULL return the documented failure
value -- apply it to every public function, document it once in
`sm.h`, and add a test that calls every public function with NULL.

**S6. Rust and Python decode safety.**  `SparseMap::from_bytes`
adds chunk spans without overflow checks at six sites.  Debug builds
panic; release builds (including the Python wheel) wrap silently,
producing maps that iterate out of order and give wrong set-operation
results.  Use checked arithmetic and return `DecodeError::Corrupt`;
reject the same structural cases as S1 so C and Rust agree on what is
valid.  Commit the `cargo-fuzz` target.  *Accept:* zero panics in a
30 min `overflow-checks = true` campaign, and the review's release-mode
wrong-answer reproducers are rejected.

**S7. Wire-format truth.**  The format is host-endian: a big-endian
writer and a little-endian reader reject each other's files.  The
header comment in `sm.h` documents the wrong magic and version, and
`docs/ARCHITECTURE.md` wrongly says the reader byte-swaps.  Make the
documentation match the code and state plainly that serialized maps
are portable only between hosts of the same byte order.  (Implementing
cross-endian reads is a wire-format change and is deferred; see
section 5.)  Pin the endianness behaviour with a test.

**S8. CI supply chain.**  The hegel job downloads `libhegel.so` and
`hegel.h` without verifying them, although `flake.nix` pins the same
files by hash -- verify against those hashes.  Pin every third-party
action to a full commit SHA with the tag in a comment.

**S9. Repository hygiene.**  `SECURITY.md` names 2.2.x as the
supported version; derive it from the current release.  The committed
`.envrc` calls `project_steering` and `project_mcp`, which exist only
in the maintainer's personal direnv library and fail for anyone else;
move them to an untracked `.envrc.local`.  `.gitignore` misses
`perf.data`, `.env*`, `*.pem`, `id_rsa*`, coverage and review build
directories, and libFuzzer output.  Meson's `testlog.json` records the
full environment of every test, so a build directory is as sensitive
as a core dump; document that in `CONTRIBUTING.md`.

**S10. Provenance.**  sparsemap is a hard fork of Christoph Rupp's
[cruppstahl/sparsemap](https://github.com/cruppstahl/sparsemap) (2014),
continued with his blessing.  The README must say so, and the source
headers must carry his copyright alongside the current one, as the
LICENSE already does.

**S11. RLE-free variant.**  A `no-rle` branch carrying the same
library with run-length encoding removed: sparse chunks only.  Same
public API; wire format stays version 2 but a reader rejects RLE
chunks it cannot represent.  Maintained as a branch, not a build
option, so neither copy grows `#ifdef`s.

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

- **Cross-endian deserialization** -- the format is host-endian (see
  S7).  Supporting byte-swapped reads is a wire-format version bump
  and a second decoder path; do it when a consumer actually moves
  serialized maps between byte orders.
- **An aligned-alloc hook in `sm_allocator_t`** -- only needed if SIMD
  work ever happens, and adding it now would be a speculative API slot.
- **`sm_shrink_to_fit`** -- no consumer has asked; `sm_owned_copy`
  covers the same need at the cost of one copy.
