# sparsemap architecture

A three-tier hierarchy: bit vectors, chunks, and a top-level map of
chunks.  This document covers the on-disk layout, encoding decisions,
and the cost model that motivated each.

## Tier 0 — bit vectors

Individual bits are stored in 64-bit words (`__sm_bitvec_t`, an alias
for `uint64_t`).  Bit *i* of a vector at position *p* corresponds to
absolute index `p * 64 + i`.

Bit vectors are *never* stored directly at the map level — they're
always wrapped in a chunk.

## Tier 1 — chunks

A chunk encodes up to 2048 consecutive bits (32 bit vectors × 64
bits each).  The first 64-bit word of a chunk is the **descriptor**;
the encoding depends on its top two bits:

### Sparse encoding (descriptor top bits ≠ `01`)

The descriptor word holds 2-bit flags for up to 32 bit vectors:

| Flag | Meaning                              | Vector stored after descriptor? |
|------|--------------------------------------|--------------------------------|
| `00` | all-zeros                            | no                             |
| `11` | all-ones                             | no                             |
| `10` | mixed (some zeros, some ones)        | yes                            |
| `01` | unused (reduces chunk capacity)      | no                             |

Only the *mixed* vectors are stored after the descriptor, in the
order they appear.  An all-zeros chunk takes 8 bytes (descriptor
only); an all-ones chunk also takes 8 bytes.  A fully mixed chunk
takes 8 + 32×8 = 264 bytes for 2048 bits — about 1 byte of metadata
per 8 bits of payload.

### RLE encoding (descriptor top bits = `01`)

A single 64-bit descriptor represents a contiguous run of set bits
starting at index 0 within the chunk:

```
  bits 63:62   = 01            (RLE flag)
  bits 61:31   = capacity      (max ~2 billion bits)
  bits 30:0    = run length    (max ~2 billion bits)
```

Bits `[0, length)` are set; bits `[length, capacity)` are unset.  RLE
chunks store no bit vectors after the descriptor — they are 8 bytes
total regardless of capacity.

Best case for the whole library is a single RLE chunk encoding
2 billion set bits in 8 bytes.

## Tier 2 — map

The top-level `sm_t` manages an ordered sequence of chunks.
Layout in the data buffer:

```
  [4 bytes: chunk count]
  [chunk 0:   4-byte start offset | 8-byte descriptor | optional vectors]
  [chunk 1:   4-byte start offset | 8-byte descriptor | optional vectors]
  ...
```

Each chunk's *start offset* is an absolute bit index, aligned to a
chunk-capacity boundary.  Chunks are stored in increasing-offset
order so binary search and merge-style algorithms work.

### Encoding transitions

- A sparse chunk whose 32 vectors are all `11` (all-ones) transitions
  to RLE when the next adjacent bit is set, extending the run beyond
  2048 bits.
- Modifying bits inside an RLE run (clearing a bit in the middle, for
  example) causes the RLE chunk to be *separated* into one or more
  sparse chunks plus (optionally) smaller RLE chunks for the
  remaining contiguous runs.
- Adjacent chunks that together form a contiguous run of set bits
  are *coalesced* into a single RLE chunk automatically.

## Lifecycle / allocation lineage

Every `sm_t` carries an internal `m_alloc_kind` tag that
records how its data buffer was provisioned:

| Lineage              | How it's set                                  | How to dispose                               |
|----------------------|-----------------------------------------------|----------------------------------------------|
| `SM_OWNED_CONTIGUOUS`| `sm_create()`, `sm_copy()`,    | `sm_free()` *or* libc `free()`        |
|                      | `sm_owned_copy()`                     |                                              |
| `SM_WRAPPED`         | `sm_wrap()`, `sm_init()`,      | `sm_free()` (caller frees buffer)     |
|                      | `sm_open()`                           |                                              |
| `SM_OWNED_SPLIT`     | promoted from `SM_WRAPPED` by                | `sm_free()` (libc free leaks!)        |
|                      | `sm_set_data_size(map, NULL, larger)` |                                              |

The lineage tag drives `sm_set_data_size`'s behavior so it can
never silently no-op a resize — see [API.md](API.md) for the full
contract and [MIGRATION.md](MIGRATION.md) for what changed in v1.

## Thread safety

Sparsemap is **not** thread-safe.  Concurrent reads of an immutable
map are safe; any writer must hold an external lock.  A lock-free /
wait-free variant is being designed in `experiment/thread-safe`; see
[../.agent/notes/sparsemap-cleanup-plan.md](../.agent/notes/sparsemap-cleanup-plan.md)
Phase 6 for the design track.
