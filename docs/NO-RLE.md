# The RLE-free ("no-rle") variant

This is the **RLE-free sibling** of sparsemap.  It is the same library
and the same public API as the mainline (RLE) build, with the
run-length-encoded chunk representation removed and a compact
**small-set** mode added for near-zero index sets.  Both variants are
maintained together; the mainline stays the RLE version.

## What changed vs. the RLE build

- **No RLE chunks are ever written.**  A run of set bits longer than one
  2048-bit chunk window is stored as a stretch of adjacent *all-ONES
  sparse* chunks (descriptor `~0`, no payload words), not as a single
  RLE descriptor.  The maximal-run iterator, the set-algebra emitter,
  `sm_offset`, and `sm_split` all produce sparse-only output.
- **RLE input is rejected, cleanly.**  The RLE variant marks a
  run-length-encoded chunk with the descriptor top-two-bits pattern
  `01`.  This build cannot represent such a chunk, so `sm_validate()`
  rejects any map that contains one; `sm_open()` and `sm_deserialize()`
  therefore return `NULL` (or, per the S1 validation contract, an empty
  map) for an RLE-encoded stream rather than crashing or mis-decoding.
  The pre-validate size walk already returns the safe 8-byte stride for
  an RLE-flagged descriptor, so the reader never over-reads before the
  reject fires.  (We do **not** expand RLE-to-sparse on read: reject is
  simpler and correct for a sibling variant.)
- A same-endian, sparse-only stream written by either variant
  round-trips through the other.

Everything else — the sparse chunk codec, the 3-word `sm_t`, the wire
header, the hardening contracts (S1–S5), the full `sm_*` surface — is
identical to the RLE build's `sm.h` (minus the RLE-specific macros).

## Small-set mode

Chunk mode carries an 8-byte chunk-count header plus, per 2048-bit
chunk, an 8-byte chunk-start and an 8-byte descriptor.  That overhead
loses to PostgreSQL's `Bitmapset` (a flat `uint64` word array from bit 0
behind an 8-byte header) for sets whose indices all sit near zero.

The no-rle variant closes that gap: when every set index is below a
small threshold **and** the small form is no larger than the equivalent
single chunk, the map is stored exactly like `Bitmapset` — a bare
`uint64 bitmapword[]` from bit 0 (bit *i* lives in word *i*/64) — behind
an 8-byte header that ties Bitmapset's 8-byte header.

### Layout

The mode is **self-describing in the `m_data` byte stream**, not in the
`sm_t` struct (which stays three machine words).  The same 8-byte header
word that holds the chunk count in chunk mode has its top bit set in
small mode, with the bitmapword count in the low 32 bits:

```
chunk mode:  [ chunk_count : 8 bytes ][ chunk 0 ][ chunk 1 ] ...
small mode:  [ 0x8000...00 | nwords : 8 bytes ][ word[0] ][ word[1] ] ... [ word[nwords-1] ]
             m_data_used = 8 + nwords * 8
```

A chunk count never approaches 2^63, so the top bit is free.  The cap is
`SM_SMALL_MAX_WORDS = 16` words (1024 bits); above that the map is always
in chunk mode.  A small-mode add prefers the chunk form whenever it is
smaller *and* fits in the current buffer in place; if a tight buffer
would force a grow just to shrink the representation, the map stays in
the small form rather than reallocating.  So the small form is normally
the smaller of the two, but a set built by ascending inserts into a
tightly-sized buffer can sit in a small form up to one word (8 bytes)
larger than its chunk equivalent.  Either way the footprint stays at or
below an equivalent `Bitmapset` (verified over two million random
near-zero sets), which is the guarantee that matters to a consumer.

### Footprint (bytes)

`Bitmapset` = 8-byte header + `ceil((maxbit+1)/64) * 8`.

| set        | small-set | chunk mode | Bitmapset |
|------------|-----------|------------|-----------|
| `{0}`      | **16**    | 32         | 16        |
| `{0..63}`  | **16**    | 24         | 16        |
| `{5,70}`   | **24**    | 40         | 24        |
| `{0,200}`  | **40**    | 40         | 40        |
| `{0,2000}` | (chunk)   | **40**     | 264       |
| `{200}`    | (chunk)   | **32**     | 40        |

Small-set ties Bitmapset on the near-zero / dense-low cases and never
exceeds it; sparsemap beats Bitmapset outright once the index span
crosses the small cap (chunk mode wins by a wide margin — 40 vs 264 at
`{0,2000}`) or when a lone high index makes the flat-from-zero
`Bitmapset` wasteful (`{200}`: 32 vs 40).

### Transitions

- **Promote** small → chunk when an added index leaves the small span,
  or when the equivalent chunk would be strictly smaller than the small
  form (a lone high index).  The chunk form is the smaller one in that
  case, so the promotion always fits in place.
- **Demote** chunk → small when removals (or a set-algebra result) bring
  every index back under the cap and the small form is no larger.

Transitions are transparent and lossless.  Every operation validates and
serialize-round-trips before and after a transition, and the small-set
and chunk representations of the same logical set compare equal via
`sm_equals` and serialize to forms each other's `sm_deserialize`
accepts.

### Every API works in both modes

The point / scalar / iteration primitives (`sm_contains`, `sm_add`,
`sm_remove`, `sm_cardinality`, `sm_minimum`, `sm_maximum`,
`sm_is_empty`, `sm_next_member`, `sm_prev_member`, `sm_get_size`,
`sm_get_data`, `sm_contains_many`) read the word array directly.  The
maximal-run iterator decodes a small map up front, so all run-based set
algebra, hashing and comparison (`sm_union`, `sm_intersection`,
`sm_difference`, `sm_xor`, `sm_extract_range`, `sm_hash`, `sm_equals`,
`sm_compare`, `sm_subset_compare`, the `*_cardinality` family,
`sm_jaccard_index`) are covered at one chokepoint.  The remaining
raw-chunk walkers (`sm_offset`, `sm_split`, `sm_scan`, `sm_statistics`,
`sm_select`, `sm_rank`, `sm_span`) operate on a transparently
materialized chunk view; `sm_split` promotes in place.  The
`sm_locator_*` accelerator returns a degenerate (always-fall-back)
locator for a small map, so its queries route to the plain,
small-set-aware path.

## Wire format

The wire format is unchanged: a 16-byte header (magic `0x30316d73`,
version `2`, endian flag, cardinality) followed by the internal `m_data`
body copied verbatim.  Because the body *is* `m_data`, a small-set map
serializes to its small-set form automatically, and `sm_deserialize`
reconstructs it (the body's own header marker is authoritative;
reserved header byte 6 is set to `1` as a documented mirror of the mode).
Small-set maps are portable between same-endian hosts, exactly like
chunk-mode maps.
