# Changelog

Notable changes per release.  The Rust port keeps its own log in
[rust/CHANGELOG.md](rust/CHANGELOG.md); versions are held in lockstep
across the C library, the Rust crate and the Python binding, so a
release exists even where one of them is functionally unchanged.

## 5.9.0 (no-rle variant)

Matches the mainline 5.9.0, re-implemented on the sparse-only code;
still zero run-length-encoding code.  No wire-format version change;
`sm_t` and `sm_cursor_t` are unchanged.

### Fixed

- `sm_select(map, n, false)` counted only the unset bits before the
  first chunk and inside chunks, so it answered too far right on any map
  with a gap, or gave up (`{8192}`: select0(8192) was SM_IDX_MAX, not
  8193).
- Chunks that reach 2^64 no longer wrap their end to 0: `sm_and`,
  `sm_andnot`, `sm_or`, `sm_xor`, `sm_add_many`, `sm_create_from_array`,
  `sm_split` and the locator lost members or built invalid maps there.
- `sm_offset` computed a chunk's shifted start in `ssize_t`, a signed
  overflow for chunks at or above 2^63 (a mainline 5.7.0 fix that never
  reached this build): a shift near the top dropped members or returned
  NULL.
- `sm_split` with the index in a gap between chunks moved the whole chunk
  below the index into `other` (also a mainline 5.7.0 fix that never
  reached this build; 1,007 of 2,000 random sparse splits were wrong),
  and a split inside a chunk past its last member left an empty chunk in
  `other` (`sm_minimum(other)` = 0).
- `sm_offset` leaves chunks with NONE slots; a later `sm_add` into the
  uncovered tail inserted a second chunk with the same start
  (`sm_validate` false).  A chunk now owns its whole 2048-bit window.
- `sm_validate` rejects a chunk with no member and a set bit 2^64 - 1;
  `sm_open`, `sm_open_copy` and `sm_deserialize` drop empty chunks from
  older streams first.
- An emptied small-set map serializes like every other empty map
  (header byte 6 is now 0).
- sm.h no longer claims a stale locator stays correct: a bit flipped
  inside a chunk is not detected.

### Changed

- SM_IDX_MAX (2^64 - 1) is not a member: the adders, `sm_remove` and the
  bulk builders return EINVAL for it, `sm_add_grow` grows only on
  ENOSPC, `sm_contains` is false for it, and `sm_offset` onto it is
  ERANGE.
- rank, select, span, scan and statistics read a small-set map's words
  instead of building a chunk-mode copy (`sm_rank`: about 17 us to
  27 ns).
- Descriptor sizes and positions are one popcount; in-word select uses
  pdep under BMI2; x86 builds without POPCNT use an inline popcount.
- `sm_remove_range`, `sm_flip_range`, `sm_overlap`, `sm_is_subset` and
  `sm_nonempty_difference` work run by run, not bit by bit.
- `sm_prev_member` uses its cursor for a descending scan.
- The locator is a two-level chunk directory (about 10 bytes per chunk):
  contains, rank and select of both values are O(log chunks).
- `sm_deserialize` reads streams written on the other byte order.
- The README, sm.h, sm.c and docs no longer compare against PostgreSQL's
  `Bitmapset` or list PostgreSQL TID sets as a good fit: measured on
  those workloads, sparsemap is neither smaller nor faster.

### Added

`sm_locator_next_member`, `sm_locator_prev_member`, `sm_rank_many`,
`sm_select_many`, `sm_next_member_op`, `sm_scan_op` (`sm_op_t`), and
`sm_freeze` / `sm_frozen_*` / `sm_thaw` (a read-only Elias-Fano copy with
its own serialized format).

### Not applicable to this build

The mainline's RLE run split, and its fix for a removal that emptied a
chunk next to an RLE run (this build's coalesce is a no-op;
`test_remove_between_runs` guards it).  A run here costs one 16-byte
chunk per 2048 bits, so `sm_add_range` of 2^31 bits needs a 16 MiB
buffer and fails with ENOSPC, map unchanged, in a smaller one.

### Wire format

5,000 maps built through the public API serialize byte-identically to
no-rle-v5.8.5, and every no-rle-v5.8.5 stream reads and reserializes
identically, apart from the two edge cases above (emptied small-set map;
streams with an empty chunk).

## 5.8.5 (no-rle variant)

Matches the mainline 5.8.5.  The read cursor's 5.8.4 gap resume trusted
an unverified left-neighbour hint here too, and pg_fts's reproducer gave
the same wrong answer on this build.  The walk now starts only at chunk
0, or at the cached offset after its start word matches.  A lookup in
the gap before a chunk caches that chunk's verified left neighbour, so
the 5.8.4 speedup stays.  `sm_next_member` now checks its cursor as well.
No API/ABI/wire change; still zero run-length-encoding code.

## 5.8.4 (no-rle variant)

Matches the mainline 5.8.4.  pg_fts's cursor fix applies here unchanged:
an ascending `sm_contains` pass with one cursor over a sparse map no
longer re-walks the chunk list from the start for each lookup that falls
in the gap before the cached chunk (100,000 such lookups over 20,000
chunks: 3,052 ms -> 3.4 ms).  The mainline's `sm_add_range` over-read
needs a chunk-merging pass this build doesn't have, so no public path
reached it here; the internal byte move it came from is clamped all the
same.  No API/ABI/wire change; still zero run-length-encoding code.

## 5.8.3 (no-rle variant)

Matches the mainline 5.8.3.  The fixes that apply to the RLE-free build:
`sm_open` / `sm_init` with a buffer shorter than the 8-byte header is
now the empty map (it read and wrote past the caller's buffer); the
small-set word array is accessed alignment-safely, so a misaligned
caller buffer is no longer undefined behaviour; and `sm_validate`'s
descriptor-capacity check is O(1) per chunk.  The mainline's RLE set-op
fixes (double-counted tail, unaligned RLE emit) do not apply -- this
build has no RLE chunks and was verified clean on those shapes; their
regression tests are included to keep it that way.  `sm_validate`
already bounds-checked before loading a chunk start here.  No
API/ABI/wire change; still zero run-length-encoding code.

## 5.8.2 (no-rle variant)

Matches the mainline 5.8.2.  The mainline fix (an `sm_difference`
clipped-RLE path emitting an unaligned chunk start) does NOT affect the
RLE-free build -- its `__sm_emit_chunk_bits` is sparse-only and already
anchors every output chunk at the aligned start, so the minimal
reproducer validates on this variant unchanged.  The regression test
(`tests/test_difference_align.c`) is added here too to lock that in.  A
dead helper clang 21 flagged as unused was removed.  No API/ABI/wire
change; still zero run-length-encoding code.

## 5.8.1 (no-rle variant)

Two bug fixes matching the mainline 5.8.1, both in the shared
sparse-chunk code: a `sm_add_many`/`sm_create_from_array` use-after-free
on an owned map that must grow, and an `sm_validate` gap that accepted a
sparse descriptor whose data-bearing slots exceed the chunk's capacity
(the slot-indexed and capacity-bounded readers then disagreed).
`sm_validate` now rejects such a chunk.  No API/ABI/wire change
(`sm_t` still 32, wire version 2).  Still zero run-length-encoding code.

## 5.8.0 (no-rle variant)

Performance release, matching the mainline 5.8.0: `sm_add_range` emits
runs instead of looping per bit, `sm_cardinality` is O(1) via a lazy
cached runtime-only field, and bulk `sm_add_many` merges runs in one
pass.  Wire format unchanged (version 2); `sizeof(struct sparsemap)`
grows 24->32 (the runtime-only count field, never serialized), so a C
consumer embedding `sm_t` by value must recompile against the new
`sm.h`.  The mainline `__sm_coalesce_map` over-read fix is a no-op here
(this variant's coalesce is a stub -- adjacent all-ones sparse chunks
need no merging).  Still zero run-length-encoding code.

## 5.7.0 (no-rle variant)

The RLE-free variant of sparsemap: sparse chunks only, no run-length
encoding anywhere in the source.  Adds the small-set representation and
tracks the mainline hardening.  Wire format version 2; a stream this
build produces is readable by the mainline build and vice-versa for
same-endian data.

### Added

- **Small-set mode.**  A map whose largest index is below 1024 is stored
  as a bare `uint64` word array from bit 0 (PostgreSQL `Bitmapset`
  layout) behind the same 8-byte header the chunk form uses, so a
  near-zero set ties `Bitmapset` (16 bytes for `{0}` or `{0..63}`, 24
  for `{5,70}`) instead of paying per-chunk overhead.  Promotes to
  sparse chunk mode above the cap, demotes on shrink; every public API
  works in either mode.

### Removed

- **All run-length-encoding code, including RLE detection.**  There is
  no `SM_RLE_*`, no `__sm_chunk_is_rle`, and no RLE concept in the
  reader.  Every chunk is sparse; a foreign stream carrying the old
  run-length descriptor pattern is read as an ordinary sparse chunk and
  bounded by structural validation (no over-read, no crash).
- The vestigial `chunks_reserved` / `bits_reserved` fields (always 0)
  are gone from `sm_stats_t`.

### Changed

- `sm.c` and `sm.h` compile with zero warnings under the strict flag set
  (`-Wall -Wextra -Wpedantic -std=c17 -Wconversion -Wsign-conversion
  -Wc90-c99-compat` and the rest).  Declarations hoisted to block scope,
  `UINT64_C`/`INT64_C` literals, explicit sign-conversion casts.

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
