# Changelog

Notable changes per release.  The Rust port keeps its own log in
[rust/CHANGELOG.md](rust/CHANGELOG.md); versions are held in lockstep
across the C library, the Rust crate and the Python binding, so a
release exists even where one of them is functionally unchanged.

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
