# Changelog

Notable changes per release.  The Rust port keeps its own log in
[rust/CHANGELOG.md](rust/CHANGELOG.md); versions are held in lockstep
across the C library, the Rust crate and the Python binding, so a
release exists even where one of them is functionally unchanged.

## 5.8.0

A performance release from an all-APIs benchmark against CRoaring and
PostgreSQL `Bitmapset`/`TIDBitmap`.  Three hot paths that were the main
weaknesses are now orders of magnitude faster, reusing the existing run
emitter and a lazy cache.  **The wire format is unchanged (still
version 2, byte-identical to 5.7.x).**  There is one ABI change:
`sizeof(struct sparsemap)` grows from 24 to 32 bytes (a new runtime-only
field, never serialized) -- a consumer that embeds `sm_t` **by value**
must recompile against the new `sm.h`; on-disk data is unaffected.

### Performance

- **`sm_add_range` is O(runs), not O(bits).**  It routed each bit
  through `sm_add`; it now emits the run through the same run merger the
  set operations use.  `sm_add_range(m, 0, 10_000_000)` drops from ~140
  ms to ~1 us, and the result stays the same compact map (a solid run
  is still 24 bytes).
- **`sm_cardinality` is O(1).**  It walked every chunk popcounting on
  each call (linear, up to ~4 ms on a million-element map).  The count
  is now cached lazily in a runtime-only field, invalidated on any
  mutation and recomputed on first read -- so `sm_cardinality` and the
  operations built on it (`*_cardinality`, `sm_jaccard_index`, the
  `sm_equals` fast path) are constant-time.  This is the 24->32 byte
  field above; it is never written to the wire.
- **Bulk `sm_add_many` / `sm_add_many_grow` merge, not insert.**  They
  sort the input into runs and interval-union-merge them in one pass via
  the run emitter.  The pathological case -- inserting a batch of sparse
  bits into an already-large map -- drops from tens of seconds to
  milliseconds (a 50k-into-50k-chunk build measured ~36 s -> ~13 ms);
  ascending appends, already amortized, are modestly faster too.

### Fixed

- **`__sm_coalesce_map` heap over-read.**  The coalesce walk was bounded
  by the stored chunk count, but a coalesce shrinks `m_data_used` while
  the count can transiently over-report, so the walk pointer could read
  one chunk past the buffer (results were correct; the read was
  undefined behaviour, caught by AddressSanitizer via the new
  `sm_add_range` path).  The walk is now bounded by `m_data_used`.  This
  is shared code every set operation routes through.

### Docs

- The `SPARSEMAP_PREFIX` documentation now explains that value macros
  (`SM_IDX_MAX`, `SM_CURSOR_INIT`, ...) cannot be renamed by the
  preprocessor for a caller-chosen prefix -- the C preprocessor cannot
  form a macro name by token-pasting in a `#define` name position -- and
  gives the one-line consumer-side alias as the escape hatch.

### Variants

- All three variants ship these changes: the RLE build (`main`), the
  Rust crate + Python binding (`ports/rust`), and the RLE-free `no-rle`
  branch.  (The `no-rle` `__sm_coalesce_map` is a no-op stub -- adjacent
  all-ones sparse chunks need no merging -- so it never had the
  over-read; the fix is a no-op there.)

## 5.7.0

Adds a small-set representation that matches or beats PostgreSQL's
`Bitmapset` for near-zero index sets, plus two correctness fixes found
while qualifying it and a warning-clean pass under a strict compiler
flag set.  No wire-format change (still version 2, mutually readable
with 5.6.x); a drop-in source swap for any 5.6.x vendored copy.

### Added

- **Small-set mode.**  A map whose largest index is below a small cap
  (1024 bits) is stored as a bare `uint64` word array from bit 0 --
  exactly PostgreSQL's `Bitmapset` layout -- behind the same 8-byte
  header the chunk form uses (the header's top bit selects the mode, so
  it costs no extra bytes).  For near-zero sets this ties `Bitmapset`
  (`{0}` and `{0..63}` are 16 bytes, `{5,70}` is 24) instead of paying
  sparsemap's per-chunk addressing overhead, while the chunk form still
  wins once indices spread.  The promote decision is RLE-aware: a dense
  low run compresses to a single ~24-byte RLE chunk rather than the
  flat form, so runs, sparse scatter and wide spreads each land in the
  smallest of the three encodings.  The map promotes to chunk mode when
  an index exceeds the cap and demotes back on shrink; every public API
  works transparently in either mode.  The RLE-free `no-rle` variant
  carries the same small-set mode over sparse chunks only.

### Fixed

- **`sm_equals` / `sm_hash` / `sm_compare` on equal-but-differently-built
  maps.**  The run iterator decomposed runs per chunk and did not
  coalesce a run ending at a chunk boundary with the run beginning at
  the next chunk.  A contiguous range stored across a seam (as
  `sm_union` of split halves produces) decomposed differently from the
  same set built by one `sm_add_range`, so two logically-equal maps
  compared unequal and hashed differently.  The iterator now yields a
  canonical, maximal run decomposition.
- **`sm_split` could emit an invalid map.**  Splitting a source whose
  moved half contains a gap between two runs produced a map with the
  correct cardinality but a mis-structured chunk stream that failed
  `sm_validate`.  Both halves are now always valid for any split point.
- **`sm_offset` signed-integer overflow.**  A large `|offset|` near
  `SSIZE_MAX` overflowed a signed intermediate even when the final
  position was in range; the shift arithmetic is now well-defined and
  correct (the positive case is computed in `uint64_t`).
- A crafted length-1 RLE chunk whose sole bit is removed no longer
  reports a full-capacity run.

### Changed

- `sm.c` and `sm.h` compile with **zero warnings** under a strict flag
  set (`-Wall -Wextra -Wpedantic -std=c17` plus `-Wconversion`,
  `-Wsign-conversion`, `-Wc90-c99-compat`, `-Wshadow`, `-Wcast-align`,
  `-Wformat=2`, `-Wdouble-promotion` and the rest).  Declarations are
  hoisted to block scope (KNF style), `ULL`/`LL` literals use
  `UINT64_C`/`INT64_C`, and sign conversions are explicit.  No behavior
  change.

## 5.6.0

A security-hardening release, from a production-readiness review that
fuzzed every decode and mutation path with untrusted input.  No
wire-format change (still version 2); a drop-in source swap for any
5.5.x vendored copy.  The governing rule is now explicit: **any byte
stream the library did not produce itself is untrusted**, whichever
entry point it arrives through.

### Security

- **One definition of a valid map, enforced at every decode entry.**
  `sm_validate` now also rejects an RLE chunk whose length exceeds its
  capacity, a chunk start not aligned to the chunk width,
  `start + capacity` overflowing `uint64_t`, chunks whose spans
  overlap, and a stored chunk count that disagrees with the walk.
  `sm_open` and `sm_open_copy` run it and reject malformed input, as
  `sm_deserialize` already did -- previously they trusted the bytes and
  a crafted chunk could crash a later `sm_add`.
- **Memory safety on valid-but-adversarial maps.**  Fixed a source-side
  over-read in `sm_split`, a non-terminating per-bit loop, a
  destination over-write, and an out-of-range shift -- all reachable
  from maps that pass validation.
- **Termination and amplification.**  A 24-byte input can legitimately
  declare a two-billion-bit run.  `sm_xor`, `sm_extract_range`,
  `sm_hash`, the `*_cardinality` family, `sm_jaccard_index`,
  `sm_equals`, `sm_compare` and `sm_subset_compare` were O(set bits)
  and would hang for seconds on such input; they now work run-by-run
  and complete in microseconds.  `sm_to_array` is inherently
  O(cardinality) and is documented as such.
- **One NULL-map contract.**  A NULL map is an empty, read-only map;
  mutators return their documented failure value and set
  `errno = EINVAL`.  Nineteen public functions previously segfaulted on
  NULL.  Documented in `sm.h` and covered for all public functions.

### Rust and Python

- `SparseMap::from_bytes` uses checked arithmetic and rejects the same
  structurally-invalid chunks the C `sm_validate` does.  Release builds
  previously wrapped silently, producing maps that iterated out of
  order and gave wrong set-operation results; the Python wheel
  inherited this.  `from_bytes` now never panics and never returns a
  corrupt map on any input, and the Python binding raises `ValueError`
  on malformed bytes.  A `cargo-fuzz` target is committed.

### Tests and tooling

- A mutating libFuzzer harness (`tests/fuzz_mutate.c`) exercises the
  write paths the read-only harness never reached.
- The CI hegel download is verified against the `flake.nix` hashes and
  third-party actions are pinned to commit SHAs.
- `SECURITY.md` tracks the current release generically; `.gitignore`
  covers profiling data, dotenv secrets, keys, coverage/review build
  directories and fuzzer output; personal direnv hooks moved to an
  untracked `.envrc.local`; `CONTRIBUTING.md` warns that a build
  directory embeds the test environment.

### Docs and provenance

- The wire-format documentation now matches the code: the format is
  host-endian and a cross-endian file is rejected, not byte-swapped.
- The README credits Christoph Rupp's
  [cruppstahl/sparsemap](https://github.com/cruppstahl/sparsemap) as
  the origin, continued with his blessing; his copyright joins the
  source headers.

### Variants

- A separate `no-rle` branch provides an RLE-free build (sparse chunks
  only) with a small-set representation that matches or beats
  PostgreSQL's `Bitmapset` for near-zero index sets and transitions to
  the chunk representation as indices spread.  The RLE build on `main`
  is unchanged and remains the default.

## 5.5.1

Internal hardening.  No API or wire-format change; a drop-in source
swap for any 5.5.0 vendored copy.

### Changed

- `__sm_append_data`, the helper every chunk write goes through, now
  returns `bool` and is marked `warn_unused_result` (`_Check_return_` on
  MSVC).  It previously returned void and recorded its capacity
  precondition only through `__sm_assert`, which expands to `((void)0)`
  unless `SPARSEMAP_DIAGNOSTIC` is defined -- so in a release build a
  caller that forgot to reserve space got a silent heap overflow.  That
  is what happened in `sm_split` before 5.5.0, and the corruption
  surfaced much later as an unrelated glibc "realloc(): invalid next
  size".  All eight call sites now propagate a failure (`false` for the
  chunk appenders, `SM_IDX_MAX` for `__sm_map_set` and `sm_split`), and
  a future caller that forgets will not compile clean.

  The recovery *policy* deliberately stays with the caller: the
  library-owned result maps in `sm_union` and friends grow their buffer,
  while operations on a caller-supplied buffer must fail with
  `errno = ENOSPC`, and a callee cannot choose between the two.
  `sm_split` additionally keeps its up-front total, because its move
  loop cannot be unwound -- with only the per-append guard a refusal
  still leaves a partially filled destination.

  Performance-neutral: `sm_union`'s object code is byte-identical and
  total text grows 88 bytes.

### Testing

`test_split_undersized_destination` pins the contract: refusal,
`ENOSPC`, source unchanged, destination empty, and that a large-enough
destination still succeeds.  Verified to fail when the guard is removed.

Coverage is unchanged at 78.8% branches / 91.0% lines / 99.3%
functions.  (An earlier 80.2% reading was an artifact of reusing a
coverage build directory, which accumulates `.gcda` counters across runs
and inflates the denominators; `measure_coverage.sh` should be pointed
at a fresh directory.)

## 5.5.0

A correctness release.  Seven bugs, three of them data-loss or
corruption on ordinary inputs, plus one new API.  Anyone vendoring an
earlier 5.x should take this one; the wire format is unchanged (still
version 2) so upgrading is a drop-in source swap.

### Fixed

- **Big-endian hosts computed wrong answers for every counting and
  navigation operation.**  The sparse chunk descriptor packs
  thirty-two 2-bit flags into one 64-bit word, so flag byte *n* is bits
  [8n, 8n+7] of the *value*.  Ten sites read those bytes by aliasing the
  word with a `uint8_t *` and incrementing, which walks the flags in
  reverse on big-endian.  `sm_contains` was unaffected because it shifts
  the word directly, so maps stored and matched correctly and the bug
  hid behind a working membership test -- but `sm_cardinality`,
  `sm_minimum`, `sm_maximum`, `sm_rank`, `sm_select` and `sm_scan` were
  all wrong.  Measured on sparcv9 before the fix: 6 of 8 test suites
  failed, and a map with bits 42 and 1024 set reported cardinality 1,
  minimum 768 and maximum 1792.  Affects every release from v1.0.0.
- **`sm_difference` silently discarded every surviving bit when both
  chunks were RLE.**  `sm_union` and `sm_intersection` each have an
  explicit both-RLE branch; difference did not, so those overlaps fell
  into a fallback whose `else` arm assumed it was unreachable and zeroed
  the word buffers.  `sm_difference([0,16384), [0,16357))` returned an
  empty map instead of the 27-bit tail; a sweep of run lengths against
  chunk multiples failed 301 of 368 cases.
- **`sm_offset` produced structurally corrupt maps.**  Several source
  pieces can shift into the same aligned output chunk, and five
  independent append sites each chose their own start, so chunks with
  duplicate start offsets were emitted.  That breaks the ascending-start
  invariant every reader assumes: `sm_validate` rejected the result,
  `sm_contains` missed bits that `sm_next_member` still yielded, and
  `sm_deserialize` refused the map's own serialized bytes.  A related
  defect widened an RLE run to cover a later one, turning a 1000-bit gap
  into 1000 spurious set bits.  All emission now goes through one
  ordered emitter; a 6696-case sweep went from 3177 failures to 0.
- **`sm_select` returned an index that was not set.**  A saturated slot
  supplies exactly 64 candidates addressed n = 0..63, but the skip-ahead
  guard tested `n > 64` instead of `n >= 64`, so n = 64 returned a
  position one past the slot it had just declined to leave.  A map with
  bits [0,128) and [500,510) answered `sm_select(128, true) = 128`
  instead of 500.  Every multiple-of-64 rank on a dense range was
  affected.
- **`sm_split` overflowed a destination that was too small** instead of
  returning `SM_IDX_MAX` with `errno = ENOSPC` as documented.  It moved
  chunks with `__sm_append_data`, which does no bounds check.
- **`sm_locator_rank` and `sm_locator_select` crashed on a NULL
  locator.**  Both wrote their fallback as
  `sm_rank((sm_t *)(loc ? loc->map : NULL), ...)`, which looks like a
  guard but hands NULL to a function that does not accept one.
- **`sm_minimum`, `sm_maximum` and `sm_select` disagreed with
  `sm_contains`** about where a `SM_PAYLOAD_NONE` slot sits, on maps
  opened from a foreign buffer.  They now advance by one vector per NONE
  slot, matching the positional addressing `sm_contains` uses.
- Two latent defects that only became reachable once `sm_offset` emitted
  valid maps: `__sm_coalesce_map` held a `const` offset of 0 while its
  pointer walked forward, spinning forever on a successful coalesce; and
  `__sm_coalesce_chunk` located the adjacent chunk with the RLE stride
  even for an all-ONES *sparse* chunk, which also carries 32 payload
  words.

### Added

- `sm_xor_inplace(dst, src)` completes the in-place set-operation
  family.  Unlike its siblings it can grow `dst`, so it follows
  `sm_union_inplace`'s reallocating contract: assign the result, and
  NULL means failure with `dst` untouched.
- `scripts/check_prefix_coverage.sh` verifies every exported `sm_*`
  symbol has a `SPARSEMAP_PREFIX` `#define`.  Nothing checked this, so a
  new public function could silently become un-renameable and collide in
  a consumer that vendors two copies.
- `scripts/check_endian_safety.sh` rejects byte-pointer aliases of a
  chunk descriptor.  A little-endian host cannot test for that
  regression -- a byte walk and a shift are identical there -- so the
  guard has to be structural.
- `scripts/gen_api_index.sh` generates the `docs/API.md` function index
  from `sm.h` and `--check`s it in CI.
- The hegel property suite now runs in CI, and the version check covers
  the man pages.

### Changed

- `docs/ROADMAP.md` replaces the retired seven-phase cleanup plan, and
  records which questions are settled (scalar-only, not thread-safe,
  version lockstep) so they stop being reopened.
- The README's SIMD section is relabelled as a decision rather than
  future work, with measurements: `-O3 -march=native` already
  auto-vectorizes five loops and buys ~6-13% on set operations.
- The README and `ARCHITECTURE.md` no longer claim a lock-free variant
  is in design; that branch was last touched in 2024.
- `ARCHITECTURE.md`'s chunk layout said 4-byte counts and offsets, which
  became 8 bytes in v4.0.0; it now also documents the serialized header.

### Testing

Branch coverage 64.8% -> 78.9%, line coverage 80.7% -> 91.2%, function
coverage 99.3%.  Part of that gap was a measurement bug:
`measure_coverage.sh` ran with default timeouts, and a test killed by
timeout never flushes its `.gcda`, so the two largest suites had been
silently missing from the report.

New: randomized and shape-crossed differential tests for all set
operations against a dense oracle, an out-of-memory sweep with a
fault-injecting allocator over six source shapes, crafted maps built
through `sm_open_copy` that reach descriptor states no code path writes,
and regression tests for each fix above -- every one verified to fail
when its fix is reverted.

Verified on x86_64 Linux (gcc and clang, plus ASan/UBSan and valgrind),
sparcv9 big-endian, FreeBSD amd64, i686 ILP32, and aarch64 / riscv64 /
s390x under qemu.

## 5.4.0 and earlier

See the git history and the release notes attached to each tag.
