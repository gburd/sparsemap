# Changelog

Notable changes per release.  The Rust port keeps its own log in
[rust/CHANGELOG.md](rust/CHANGELOG.md); versions are held in lockstep
across the C library, the Rust crate and the Python binding, so a
release exists even where one of them is functionally unchanged.

## 5.8.2

A performance fix for 5.8.1's validator, two pre-existing memory-safety
fixes found while qualifying it, set-operation encoding fixes, and a
fuzz-build fix.  No API, ABI or wire-format change: `sizeof(struct
sparsemap)` is unchanged (32), the wire format is still version 2 and
byte-identical, and this is a drop-in source swap for any 5.8.x vendored
copy.  This release is the C library only.  The Rust crate and Python
binding (`ports/rust`, which embeds `sm.c`) and the RLE-free `no-rle`
variant stay at 5.8.1 for now and do not yet carry these changes.

### Performance

- **Check (f) in `sm_validate` is O(1) per chunk; 5.8.0 decode speed is
  back.**  5.8.1's descriptor-shape check (a sparse chunk's highest
  `ONES`/`MIXED` slot must fit its capacity) looped over all 32 slots of
  every chunk on every `sm_open`, `sm_open_copy` and `sm_deserialize`.
  `ONES` (`0b11`) and `MIXED` (`0b10`) are exactly the flags with the
  high bit set, so the highest data-bearing slot is the top set bit of
  `desc & 0xAAAA...AAAA`, halved: one mask and one count-leading-zeros.
  A `_Static_assert` pins the flag encoding this relies on.  Accept and
  reject behaviour is unchanged.  This was checked exhaustively over all
  2^32 high-bit patterns x 34 capacity classes (146 028 888 064
  comparisons, 0 mismatches) and on 825 664 corrupted real maps, which
  5.8.1 and 5.8.2 accept or reject identically through `sm_validate`,
  `sm_open_copy` and `sm_deserialize`.  Median of 15 alternating rounds
  pinned to one CPU (Xeon 8488C, gcc 12 `-O2`):

  | map (chunks) | `sm_open_copy` 5.8.0 | 5.8.1 | 5.8.2 |
  |---|---|---|---|
  | sparse (20 000) | 683.5 us | 1195 us (+74.8 %) | 699.3 us (+2.3 %) |
  | dense (4 000) | 136.6 us | 238.8 us (+74.8 %) | 139.7 us (+2.3 %) |
  | mixed (49 042) | 1690 us | 2946 us (+74.3 %) | 1731 us (+2.4 %) |
  | RLE (2 000 runs) | 10.12 us | 10.12 us | 8.79 us (-13.2 %) |

  `sm_validate` alone goes from +179 % to +5.5 % over 5.8.0 on sparse
  maps; that is (f) now doing real work at O(1) per chunk.  RLE-only
  maps never reach (f).  Their 5.8.2 speed-up appeared with the
  short-buffer fix below, which changes `sm_create`/`sm_clear`, and was
  not investigated further; do not count on it.

### Fixed

- **`sm_open` / `sm_init` with a buffer shorter than the 8-byte header
  (memory safety).**  A size of 0..7 rounds down to capacity 0, yet
  `sm_open` still read the 8-byte chunk-count header (an 8-byte
  heap-buffer-overflow READ), and `sm_init` wrote it (an 8-byte
  overflow WRITE).  For size 0, `sm_open` also left `m_data_used = 8`, so
  a later `sm_cardinality` walked a header the caller never supplied.
  `sm_wrap(NULL, 0)` + `sm_open(m, NULL, 0)` dereferenced NULL.  Such a
  buffer is now the empty map: `m_data_used = 0`, and nothing is read or
  written.  The fix also covers three readers (`sm_contains` via the
  chunk locator, `sm_prev_member`, `sm_select`) and `sm_copy`, which
  formed or copied from `m_data + 8` before checking for an empty map;
  that is undefined behaviour when `m_data` is NULL.  `sm_open_copy` and
  `sm_deserialize` over-allocate and were not affected.  Present since
  at least 5.8.0.
- **`sm_validate` read a chunk's start before its bounds check.**  A
  stored chunk count that over-claims made the chunk loop load 8 bytes
  past `m_data_used` before rejecting.  The public decoders' slack hid
  it; it was reachable by writing a bad count into a library-owned map
  and calling `sm_validate` directly.  The load now follows the check.
- **`sm_union` / `sm_difference` emitted two chunks with one start.**
  When an RLE chunk ended inside a sparse chunk that shares its start,
  the merge emitted the whole sparse chunk combined with the run, then
  emitted the sparse chunk's tail again at the same start.  The result
  failed `sm_validate` and `sm_cardinality` counted the tail twice; for
  example `sm_union(sm_intersection(a, b), c)` where the intersection is
  the run `[0, 164)` and `c` has bits in `[0, 2048)`.  The sparse side is
  now consumed by that emit.  Present in 5.8.0 and 5.8.1.  Membership
  answers (`sm_contains`) were already correct.
- **Set operations wrote RLE chunks at unaligned starts and with
  unaligned capacities.**  A run clipped by `sm_difference` (and the
  both-RLE paths of `sm_union` and `sm_intersection`) went out as one
  RLE chunk at the clip point with capacity == length.
  `sm_difference([10000, 20000), [10000, 13794))` returned a chunk
  starting at 13794, and `sm_difference([0, 5000), [0, 100))` one at
  100.  Check (b) in `sm_validate` requires starts that are multiples of
  2048, so it returned false and `sm_open_copy` / `sm_deserialize`
  rejected a serialized copy.  An unaligned capacity, such as `[0, 2149)`
  stored as capacity 2149, let a later `sm_add` in `[2149, 4096)` insert
  a sparse chunk at 2048, inside the run's span (check (d), overlap).  A
  clipped run is now split at chunk boundaries: whole chunks as RLE, and
  a sub-chunk head or tail as a sparse chunk merged into any output
  already in that window, the way `sm_xor` and `sm_offset` already
  emit.  Present in 5.8.0 and 5.8.1.  Membership answers were already
  correct.  `sm_difference` of two large RLE+sparse maps is about 4 %
  slower (24.4 ms vs 23.4 ms); `sm_union` is unchanged.

### Build

- **`-Dfuzz=enabled` now instruments the library, not just the
  harnesses.**  `libsparsemap.a` was built without coverage or
  sanitizers (0 `__sanitizer_cov` / `__asan` relocations), so libFuzzer
  saw only the harness: coverage stalled at 11 edges (`fuzz_deserialize`)
  and 21 (`fuzz_mutate`).  With fuzz enabled, clang compiles `sm.c` with
  `-fsanitize=fuzzer-no-link,address,undefined`, which adds 4138
  `__sanitizer_cov` relocations.  In 10-minute runs from
  `tests/fuzz-corpus`, `fuzz_deserialize` now reaches 1793 edges (3.7 M
  executions) and `fuzz_mutate` 4642 (1.9 M), with 0 crashes.  Builds
  without `-Dfuzz` are unchanged.

### Tests

- `tests/test_open_short.c` -- `sm_wrap` + `sm_open` and `sm_init` on
  exact-size buffers of 0..8 bytes and on NULL: the map is empty to
  every reader, serializes as the empty map, refuses `sm_add` with
  `ENOSPC`, and none of the caller's bytes are touched.  It fails under
  AddressSanitizer on 5.8.1.
- `tests/test_validate.c` -- a library-owned map whose header claims
  one chunk it does not hold must be rejected without an out-of-bounds
  read.
- `tests/test_setop_rle_sparse.c` -- a run (lengths 1..2100 at three
  bases) against a sparse chunk sharing its start, for union,
  intersection, difference and xor in both operand orders.  Each result
  is checked against a bit-array model; 60 failures on 5.8.1.
  It also pins the reported unaligned-start cases, checks `sm_add`
  into the gap after a difference, and runs a randomized union /
  intersection / difference / xor differential.  That differential is
  3 seeds x 4000 iterations over operands built from runs (some >= 2048
  bits and crossing chunk boundaries, some starting at 0) plus sparse
  bits.  Every result is checked for validity and exact membership,
  then used as an operand and after `sm_add`.  Without the unaligned-RLE
  fix above it reports 3040 failing checks; with it, 0.

## 5.8.1

Two bug fixes from a downstream consumer's (pg_weave) property testing.
No API, ABI, or wire-format change: `sizeof(struct sparsemap)` is
unchanged (32), the wire format is still version 2 and byte-identical,
and this is a drop-in source swap for any 5.8.0 vendored copy.

### Fixed

- **`sm_add_many` / `sm_create_from_array` use-after-free (memory
  safety).**  `sm_add_many` on an owned (`sm_create`'d) map that had to
  grow routed through `__sm_replace_buffer` -> `sm_set_data_size`, which
  reallocs the whole contiguous block (the `sm_t` included), freeing the
  caller's pointer -- yet `sm_add_many` returned `false`, whose
  documented meaning is "unchanged, retry with `sm_add_many_grow`", so a
  caller holding the now-freed pointer had a use-after-free.
  `sm_create_from_array` made it unconditional for inputs larger than its
  1 KiB seed: it then `sm_free`'d the stale pointer (a double free) and
  returned `NULL`, leaking the correct result.  `sm_add_many` now keeps
  its no-relocate contract -- on an owned map whose result would not fit
  the existing capacity it frees the scratch result and returns `false`
  leaving the caller's map valid and unchanged -- and
  `sm_create_from_array` uses the growing `sm_add_many_grow`.  Only the
  owned-must-grow path (previously the faulty one) changes behaviour.
- **`sm_validate` accepted a sparse descriptor the readers disagreed
  on.**  A sparse chunk's descriptor is 32 two-bit flags; a
  `SM_PAYLOAD_NONE` flag reduces the chunk's capacity.  `sm_validate`
  did not check that every data-bearing flag (`ONES`/`MIXED`) fits within
  that reduced capacity, so a crafted or corrupt buffer could place a
  data slot above the capacity: `sm_cardinality`/`sm_rank` (slot-indexed)
  counted it while `sm_contains`/`sm_next_member` (capacity-bounded)
  did not -- the two reader families answered inconsistently, and a
  single-byte flip could make a 2000-member map validate as claiming
  hundreds of millions.  `sm_validate` now rejects a sparse chunk whose
  highest `ONES`/`MIXED` slot extends past its capacity.  The encoder
  never writes such a chunk (verified against the full test suite and a
  randomized set-operation stress: tens of thousands of real outputs,
  zero false rejections), so no sparsemap-written buffer is affected.
  (Note: `NONE` legitimately appears interleaved among data slots in
  encoder output; the invariant is a capacity bound, not a flag-ordering
  rule.)

### Tests

- `tests/test_add_many_owned.c` -- the owned-grow path returns `false`
  with the caller's map intact, `sm_create_from_array` is
  AddressSanitizer-clean on inputs that exceed the seed capacity.
- `tests/test_validate_descriptor.c` -- every interior data-slot-past-
  capacity corruption is rejected, and for every buffer `sm_validate`
  accepts, `sm_cardinality` equals an `sm_next_member` walk.

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
