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
  [8 bytes: chunk count]
  [chunk 0:   8-byte start offset | 8-byte descriptor | optional vectors]
  [chunk 1:   8-byte start offset | 8-byte descriptor | optional vectors]
  ...
```

Each chunk's *start offset* is an absolute bit index, aligned to a
chunk-capacity boundary.  Chunks are stored in increasing-offset
order so binary search and merge-style algorithms work.  Duplicate
or out-of-order start offsets are a corrupt map: `sm_validate()`
rejects them, and the readers assume the ordering.

The count and the start offsets were 4 bytes each before v4.0.0,
which silently truncated any index at or above 2^32.  Both are
`__sm_idx_t` (a `uint64_t`) now; `SM_SIZEOF_OVERHEAD` is
`sizeof(__sm_idx_t)`, so it is the single place that width is
decided.

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

## Serialized wire format (version 2)

`sm_serialize` / `sm_deserialize` write a self-describing stream,
unlike the in-memory buffer, which is host-order and not portable.

```
  offset 0   [4 bytes]  magic 0x30316d73 ("sm10", little-endian)
  offset 4   [1 byte ]  format version (currently 2)
  offset 5   [1 byte ]  flags; bit 0 set = little-endian writer
  offset 6   [2 bytes]  reserved, zero
  offset 8   [8 bytes]  cardinality (set-bit count)
  offset 16  [...]      the chunk stream described above
```

The 16-byte header is `SM_WIRE_HEADER_LEN`.  Version 1 used 4-byte
chunk counts and start offsets; version 2 widened both to 8 bytes to
fix silent truncation at indices >= 2^32.  A reader checks the magic,
rejects an unknown version, and byte-swaps the body when the writer's
endianness flag disagrees with the host.

The C and Rust implementations are not required to produce
byte-identical streams, only mutually readable ones; CI exchanges
fixtures in both directions to enforce that.

## Thread safety

Sparsemap is **not** thread-safe, and there is no plan to make it so.
Concurrent reads of an unmutated map are safe; any writer needs an
external lock.  The `experiment/thread-safe` branch explored a
lock-free variant in 2024 and was not pursued -- treat it as an
archive.  See [ROADMAP.md](ROADMAP.md) for the settled decisions.
