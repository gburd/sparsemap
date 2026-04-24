/*-------------------------------------------------------------------------
 *
 * bitmapset.c
 *	  Hybrid Bitmapset: PostgreSQL Bitmapset API with dual-mode storage
 *
 * Dense mode:   Traditional word-array storage with uint32_t nwords header
 *               and uint8_t data[] flexible array, for sets up to 262143.
 *
 * Chunked mode: RLE + sparse chunk encoding (from sparsemap) for large or
 *               sparse sets.  Chunks are stored inline in the data[] buffer
 *               with the nwords field split into (alloc_chunks << 16 | used).
 *
 * Copyright (c) 2003-2026, PostgreSQL Global Development Group
 * Chunked storage: Copyright (c) 2024-2026, Gregory Burd <greg@burd.me>
 *
 *-------------------------------------------------------------------------
 */

#ifdef BUILDING_OUTSIDE_POSTGRES
#include "postgres_compat.h"
#else
#include "postgres.h"
#endif

#include "bitmapset.h"

#include <string.h>

/* ----------------------------------------------------------------
 * Local macros
 * ---------------------------------------------------------------- */

#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

/*
 * Isolate rightmost one-bit using two's complement trick.
 */
#define RIGHTMOST_ONE(x) ((signedbitmapword) (x) & -((signedbitmapword) (x)))

#define HAS_MULTIPLE_ONES(x)	((bitmapword) RIGHTMOST_ONE(x) != (x))

/* ----------------------------------------------------------------
 * Chunked mode constants
 * ---------------------------------------------------------------- */

#define BMS_DENSE_MAX_NWORDS  4096
#define BMS_DENSE_MAX_BIT     ((int64_t)BMS_DENSE_MAX_NWORDS * BITS_PER_BITMAPWORD - 1)
#define BMS_CHUNKED_ALLOC_UNIT 64

/*
 * BMS_DENSE_MAX_NWORDS must be less than 65536 so that dense nwords values
 * never have bits set in the upper 16 bits, which would be misidentified
 * as chunked mode by BMS_IS_CHUNKED().
 */
#ifdef BUILDING_OUTSIDE_POSTGRES
_Static_assert(BMS_DENSE_MAX_NWORDS < 65536,
               "BMS_DENSE_MAX_NWORDS must be < 65536 for mode detection");
#else
StaticAssertDecl(BMS_DENSE_MAX_NWORDS < 65536,
                 "BMS_DENSE_MAX_NWORDS must be < 65536 for mode detection");
#endif

/* ----------------------------------------------------------------
 * Chunk codec types and constants
 *
 * Inlined from chunk_codec.h with SM_* -> BMS_* renames.
 * ---------------------------------------------------------------- */

typedef struct {
	bitmapword *m_data;
} BmsChunk;

enum
{
	/* metadata overhead: sizeof(uint64_t) bytes for chunk count */
	BMS_CHUNK_OVERHEAD = sizeof(uint64_t),

	/* number of flags that can be stored in a single index byte */
	BMS_FLAGS_PER_INDEX_BYTE = 4,

	/* number of flags that can be stored in the index */
	BMS_FLAGS_PER_INDEX = sizeof(bitmapword) * BMS_FLAGS_PER_INDEX_BYTE,

	/* maximum capacity of a chunk (in bits) */
	BMS_CHUNK_MAX_CAPACITY = BITS_PER_BITMAPWORD * BMS_FLAGS_PER_INDEX,

	/* payload is all zeros (2#00) */
	BMS_PAYLOAD_ZEROS = 0,

	/* payload is all ones (2#11) */
	BMS_PAYLOAD_ONES = 3,

	/* payload is mixed (2#10) */
	BMS_PAYLOAD_MIXED = 2,

	/* vector is not used (2#01) */
	BMS_PAYLOAD_NONE = 1,

	/* a mask for checking flags (2 bits, 2#11) */
	BMS_FLAG_MASK = 3,
};

/* Flag manipulation macros */
#define BMS_CHUNK_GET_FLAGS(data, at) \
	((((data)) & ((bitmapword)BMS_FLAG_MASK << ((at)*2))) >> ((at)*2))
#define BMS_CHUNK_SET_FLAGS(data, at, to) \
	((data) = ((data) & ~((bitmapword)BMS_FLAG_MASK << ((at)*2))) | ((bitmapword)(to) << ((at)*2)))

/*
 * RLE (Run-Length Encoding) Format
 *
 * Bits 63:62 = 01 (RLE flag, matches BMS_PAYLOAD_NONE to distinguish from sparse)
 * Bits 61:31 = Chunk capacity in bits (31 bits)
 * Bits 30:0  = Run length in bits (31 bits)
 */
#define BMS_RLE_FLAGS            0x4000000000000000ULL
#define BMS_RLE_FLAGS_MASK       0xC000000000000000ULL
#define BMS_RLE_CAPACITY_MASK    0x3FFFFFFF80000000ULL
#define BMS_RLE_LENGTH_MASK      0x7FFFFFFFULL

/* ----------------------------------------------------------------
 * Chunk codec static functions
 * ---------------------------------------------------------------- */

static inline bool
bms_chunk_is_rle(const BmsChunk *chunk)
{
	const bitmapword w = chunk->m_data[0];
	return (w & BMS_RLE_FLAGS_MASK) == BMS_RLE_FLAGS;
}

static inline size_t
bms_chunk_rle_get_capacity(const BmsChunk *chunk)
{
	bitmapword w = chunk->m_data[0] & (bitmapword)BMS_RLE_CAPACITY_MASK;
	w >>= 31;
	return w;
}

static inline size_t
bms_chunk_rle_get_length(const BmsChunk *chunk)
{
	const bitmapword w = chunk->m_data[0] & (bitmapword)BMS_RLE_LENGTH_MASK;
	return w;
}

static inline size_t
bms_chunk_calc_vector_size(const uint8_t b)
{
	static int lookup[] = {
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
		1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
		2,  2,  3,  2,  2,  2,  3,  2,  3,  3,  4,  3,  2,  2,  3,  2,
		1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
		1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
		0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0
	};

	return lookup[b];
}

static inline size_t
bms_chunk_get_position(const BmsChunk *chunk, size_t bv)
{
	size_t position = 0;
	uint8_t *p = (uint8_t *)chunk->m_data;

	if (!bms_chunk_is_rle(chunk))
	{
		const size_t num_bytes = bv / ((size_t)BMS_FLAGS_PER_INDEX_BYTE * BITS_PER_BITMAPWORD);
		for (size_t i = 0; i < num_bytes; i++, p++)
		{
			position += bms_chunk_calc_vector_size(*p);
		}

		bv -= num_bytes * BMS_FLAGS_PER_INDEX_BYTE;
		for (size_t i = 0; i < bv; i++)
		{
			const size_t flags = BMS_CHUNK_GET_FLAGS(*chunk->m_data, i);
			if (flags == BMS_PAYLOAD_MIXED)
			{
				position++;
			}
		}
	}

	return position;
}

static inline size_t
bms_chunk_get_size(const BmsChunk *chunk)
{
	size_t size = sizeof(bitmapword);
	if (unlikely(bms_chunk_is_rle(chunk)))
		return size;

	uint8_t *p = (uint8_t *)chunk->m_data;
	for (size_t i = 0; i < sizeof(bitmapword); i++, p++)
	{
		size += sizeof(bitmapword) * bms_chunk_calc_vector_size(*p);
	}
	return size;
}

static inline size_t
bms_chunk_get_capacity(const BmsChunk *chunk)
{
	if (unlikely(bms_chunk_is_rle(chunk)))
	{
		return bms_chunk_rle_get_capacity(chunk);
	}

	size_t capacity = BMS_CHUNK_MAX_CAPACITY;
	uint8_t *p = (uint8_t *)chunk->m_data;

	for (size_t i = 0; i < sizeof(bitmapword); i++, p++)
	{
		if (!*p || *p == 0xff)
		{
			continue;
		}
		for (int j = 0; j < BMS_FLAGS_PER_INDEX_BYTE; j++)
		{
			const size_t flags = BMS_CHUNK_GET_FLAGS(*p, j);
			if (flags == BMS_PAYLOAD_NONE)
			{
				capacity -= BITS_PER_BITMAPWORD;
			}
		}
	}
	return capacity;
}

static inline bool
bms_chunk_is_set(const BmsChunk *chunk, const size_t idx)
{
	if (unlikely(bms_chunk_is_rle(chunk)))
	{
		if (idx < bms_chunk_rle_get_length(chunk))
		{
			return true;
		}
		return false;
	}
	const size_t bv = idx / BITS_PER_BITMAPWORD;
	Assert(bv < BMS_FLAGS_PER_INDEX);

	const size_t flags = BMS_CHUNK_GET_FLAGS(*chunk->m_data, bv);
	switch (flags)
	{
	case BMS_PAYLOAD_ZEROS:
	case BMS_PAYLOAD_NONE:
		return false;
	case BMS_PAYLOAD_ONES:
		return true;
	default:
		Assert(flags == BMS_PAYLOAD_MIXED);
		break;
	}

	const bitmapword w = chunk->m_data[1 + bms_chunk_get_position(chunk, bv)];
	return (w & ((bitmapword)1 << (idx % BITS_PER_BITMAPWORD))) != 0;
}

static inline uint64_t
bms_get_chunk_aligned_offset(const size_t idx)
{
	const size_t capacity = BMS_CHUNK_MAX_CAPACITY;
	return idx / capacity * capacity;
}

static inline void
bms_expand_sparse_chunk(const BmsChunk *chunk, bitmapword words[32], int cap_flags[32])
{
	const bitmapword desc = chunk->m_data[0];

	int vec_offsets[BMS_FLAGS_PER_INDEX];
	int running = 0;
	for (int i = 0; i < (int)BMS_FLAGS_PER_INDEX; i++)
	{
		vec_offsets[i] = running;
		running += (((desc >> (i * 2)) & BMS_FLAG_MASK) == BMS_PAYLOAD_MIXED);
	}

	for (int i = 0; i < (int)BMS_FLAGS_PER_INDEX; i++)
	{
		const unsigned f = (desc >> (i * 2)) & BMS_FLAG_MASK;
		cap_flags[i] = (f != BMS_PAYLOAD_NONE);
		words[i] = (f == BMS_PAYLOAD_MIXED) ? chunk->m_data[1 + vec_offsets[i]]
				 : (f == BMS_PAYLOAD_ONES)  ? ~(bitmapword)0
				 :                            0;
	}
}

/*
 * Encode expanded 32-word representation into a sparse chunk descriptor
 * and vector array.  Returns true if any bits are set.
 *
 * NB: This function may modify words[31] and cap_flags[31] to force
 * slot 31 to ZEROS (rather than NONE) to avoid collision with the RLE
 * flag encoding.  Callers must not rely on these arrays being unchanged.
 */
static inline bool
bms_encode_sparse_chunk(bitmapword words[32], int cap_flags[32],
						bitmapword *out_desc, bitmapword out_vecs[32], int *out_nvecs)
{
	/*
	 * Slot 31 (the highest 2-bit position in the descriptor word) must never
	 * have flags == NONE (0b01), because a descriptor word ending in 0b01 at
	 * the top would collide with the RLE encoding sentinel.  Force it to
	 * ZEROS (0b00) instead, which is semantically equivalent for an unused slot.
	 */
	if (!cap_flags[BMS_FLAGS_PER_INDEX - 1])
	{
		cap_flags[BMS_FLAGS_PER_INDEX - 1] = 1;
		words[BMS_FLAGS_PER_INDEX - 1] = 0;
	}

	bitmapword desc = 0;
	bool has_bits = false;
	unsigned flags[BMS_FLAGS_PER_INDEX];
	for (int i = 0; i < (int)BMS_FLAGS_PER_INDEX; i++)
	{
		unsigned f;
		if (!cap_flags[i])
		{
			f = BMS_PAYLOAD_NONE;
		}
		else if (words[i] == 0)
		{
			f = BMS_PAYLOAD_ZEROS;
		}
		else if (words[i] == ~(bitmapword)0)
		{
			f = BMS_PAYLOAD_ONES;
			has_bits = true;
		}
		else
		{
			f = BMS_PAYLOAD_MIXED;
			has_bits = true;
		}
		flags[i] = f;
		desc |= (bitmapword)f << (i * 2);
	}

	int nvecs = 0;
	for (int i = 0; i < (int)BMS_FLAGS_PER_INDEX; i++)
	{
		if (flags[i] == BMS_PAYLOAD_MIXED)
		{
			out_vecs[nvecs++] = words[i];
		}
	}

	*out_desc = desc;
	*out_nvecs = nvecs;
	return has_bits;
}

/* ----------------------------------------------------------------
 * Word-level operations (scalar; the compiler auto-vectorizes these)
 * ---------------------------------------------------------------- */

static inline void
bms_words_or(bitmapword dst[32], const bitmapword a[32], const bitmapword b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] | b[i];
}

static inline void
bms_words_and(bitmapword dst[32], const bitmapword a[32], const bitmapword b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] & b[i];
}

static inline void
bms_words_andnot(bitmapword dst[32], const bitmapword a[32], const bitmapword b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] & ~b[i];
}

/* ----------------------------------------------------------------
 * Chunked mode helper functions
 * ---------------------------------------------------------------- */

/*
 * Read the start offset from a chunk entry in the buffer.
 * Each entry is: [start: 8 bytes][descriptor: 8 bytes][vectors: 0-32 * 8 bytes]
 */
static inline uint64_t
bms_read_chunk_start(const uint8_t *p)
{
	uint64_t start;
	memcpy(&start, p, sizeof(uint64_t));
	return start;
}

/*
 * Write the start offset into a chunk entry in the buffer.
 */
static inline void
bms_write_chunk_start(uint8_t *p, uint64_t start)
{
	memcpy(p, &start, sizeof(uint64_t));
}

/*
 * Initialize a BmsChunk to point at the descriptor within a chunk entry.
 * The descriptor starts at offset BMS_CHUNK_OVERHEAD (8) from the entry start.
 *
 * Alignment of chunk descriptors to 8 bytes is structurally guaranteed:
 *   - data[] starts at offset 8 (sizeof(nwords) + sizeof(_padding))
 *   - BMS_CHUNK_OVERHEAD = sizeof(uint64_t) = 8
 *   - Each chunk entry = 8 + (1 + nvecs) * 8, always a multiple of 8
 * The Assert below is a debug safety net for this invariant.
 */
#ifdef BUILDING_OUTSIDE_POSTGRES
_Static_assert(BMS_CHUNK_OVERHEAD == sizeof(uint64_t),
			   "chunk overhead must equal uint64_t for alignment");
#else
StaticAssertDecl(BMS_CHUNK_OVERHEAD == sizeof(uint64_t),
				 "chunk overhead must equal uint64_t for alignment");
#endif

static inline void
bms_init_chunk_at(const uint8_t *p, BmsChunk *c)
{
	Assert(((uintptr_t)(p + BMS_CHUNK_OVERHEAD) % sizeof(bitmapword)) == 0);
	c->m_data = (bitmapword *)(p + BMS_CHUNK_OVERHEAD);
}

/*
 * Total bytes occupied by a chunk entry: start (8) + chunk data (desc + vecs).
 */
static inline size_t
bms_chunk_entry_bytes(const uint8_t *p)
{
	BmsChunk c;
	bms_init_chunk_at(p, &c);
	return BMS_CHUNK_OVERHEAD + bms_chunk_get_size(&c);
}

/*
 * Total used bytes across all chunk entries in the buffer.
 * This walks all used_chunks entries - O(n) but n is typically small.
 */
static inline size_t
bms_chunked_used_bytes(const Bitmapset *a)
{
	unsigned used = BMS_USED_CHUNKS(a);
	const uint8_t *p = BMS_BUF(a);
	size_t total = 0;

	for (unsigned i = 0; i < used; i++)
	{
		size_t entry_sz = bms_chunk_entry_bytes(p);
		total += entry_sz;
		p += entry_sz;
	}
	return total;
}

/*
 * Allocated buffer size for chunked mode.
 */
static inline size_t
bms_chunked_alloc_size(const Bitmapset *a)
{
	return (size_t)BMS_ALLOC_CHUNKS(a) * BMS_CHUNKED_ALLOC_UNIT;
}

/*
 * Find the chunk whose aligned start matches 'aligned'.
 * Returns the chunk index (0-based) or -1 if not found.
 * On success, *buf_off is set to the byte offset of that entry in the buffer.
 * On failure, *buf_off is set to the insertion point.
 *
 * This is a linear scan with early termination (chunks are sorted by start).
 * Binary search is not possible because chunk entries are variable-length,
 * so byte offsets cannot be computed without walking from the beginning.
 */
static inline int
bms_find_chunk(const Bitmapset *a, uint64_t aligned, size_t *buf_off)
{
	unsigned used = BMS_USED_CHUNKS(a);
	const uint8_t *base = BMS_BUF(a);
	const uint8_t *p = base;

	for (unsigned i = 0; i < used; i++)
	{
		uint64_t start = bms_read_chunk_start(p);
		if (start == aligned)
		{
			*buf_off = (size_t)(p - base);
			return (int)i;
		}
		if (start > aligned)
		{
			/* Chunks are sorted by start; we've passed where it would be */
			*buf_off = (size_t)(p - base);
			return -1;
		}
		p += bms_chunk_entry_bytes(p);
	}
	/* Would be inserted at the end */
	*buf_off = (size_t)(p - base);
	return -1;
}

/*
 * Like bms_find_chunk, but starts scanning from a known position.
 * 'start_idx' is the chunk index to start from, and '*buf_off' must be
 * initialized to the byte offset of that chunk's entry on entry.
 * This avoids re-scanning from the beginning when iterating in order.
 *
 * Returns the chunk index (>= 0) if found, or a negative value if not
 * found.  When not found, the return value encodes the insertion index
 * as -(insertion_index + 1), and *buf_off is set to the insertion point.
 */
static inline int
bms_find_chunk_from(const Bitmapset *a, uint64_t aligned,
					size_t *buf_off, unsigned start_idx)
{
	unsigned used = BMS_USED_CHUNKS(a);
	const uint8_t *base = BMS_BUF(a);
	const uint8_t *p = base + *buf_off;

	for (unsigned i = start_idx; i < used; i++)
	{
		uint64_t start = bms_read_chunk_start(p);
		if (start == aligned)
		{
			*buf_off = (size_t)(p - base);
			return (int)i;
		}
		if (start > aligned)
		{
			*buf_off = (size_t)(p - base);
			return -(int)(i + 1);
		}
		p += bms_chunk_entry_bytes(p);
	}
	*buf_off = (size_t)(p - base);
	return -(int)(used + 1);
}

/*
 * Ensure the chunked buffer has room for 'needed' additional bytes
 * beyond 'total_used'. May realloc; caller must refresh pointers.
 * Returns the (possibly reallocated) bitmapset.
 */
static inline Bitmapset *
bms_chunked_ensure(Bitmapset *a, size_t total_used, size_t needed)
{
	size_t alloc_sz = bms_chunked_alloc_size(a);

	if (total_used + needed <= alloc_sz)
		return a;

	/* Grow by at least 2x, rounding up to BMS_CHUNKED_ALLOC_UNIT */
	size_t new_alloc = alloc_sz * 2;
	if (new_alloc < total_used + needed)
		new_alloc = total_used + needed;

	/* Round up to next multiple of BMS_CHUNKED_ALLOC_UNIT */
	new_alloc = ((new_alloc + BMS_CHUNKED_ALLOC_UNIT - 1) / BMS_CHUNKED_ALLOC_UNIT) * BMS_CHUNKED_ALLOC_UNIT;

	unsigned new_alloc_chunks = (unsigned)(new_alloc / BMS_CHUNKED_ALLOC_UNIT);
	if (new_alloc_chunks > 0xFFFF)
		new_alloc_chunks = 0xFFFF;

	unsigned used = BMS_USED_CHUNKS(a);
	a = (Bitmapset *) repalloc(a, offsetof(Bitmapset, data) + new_alloc_chunks * BMS_CHUNKED_ALLOC_UNIT);
	a->nwords = (new_alloc_chunks << 16) | used;
	return a;
}

/*
 * Expand any chunk (RLE or sparse) to the 32-word representation.
 */
static inline void
bms_expand_chunk_words(const BmsChunk *chunk, bitmapword words[32], int cap_flags[32])
{
	if (bms_chunk_is_rle(chunk))
	{
		size_t len = bms_chunk_rle_get_length(chunk);
		size_t cap = bms_chunk_rle_get_capacity(chunk);
		for (int i = 0; i < 32; i++)
		{
			size_t bit_start = (size_t)i * 64;
			cap_flags[i] = (bit_start < cap) ? 1 : 0;
			if (bit_start + 64 <= len)
				words[i] = ~(uint64_t)0;
			else if (bit_start >= len)
				words[i] = 0;
			else
				words[i] = ((uint64_t)1 << (len - bit_start)) - 1;
		}
	}
	else
	{
		bms_expand_sparse_chunk(chunk, words, cap_flags);
	}
}

/*
 * Write encoded chunk data (descriptor + vectors) into buffer at p.
 * Returns the number of bytes written.
 */
static inline size_t
bms_write_chunk_data(uint8_t *p, bitmapword desc, bitmapword vecs[32], int nvecs)
{
	bitmapword *dp = (bitmapword *)p;
	dp[0] = desc;
	for (int i = 0; i < nvecs; i++)
		dp[1 + i] = vecs[i];
	return (size_t)(1 + nvecs) * sizeof(bitmapword);
}

/*
 * Replace a chunk in-place in the buffer. Handles resizing if the new entry
 * is a different size than the old one.
 */
static inline Bitmapset *
bms_replace_chunk(Bitmapset *a, size_t buf_off, size_t old_entry_size,
				  uint64_t start, bitmapword desc,
				  bitmapword vecs[32], int nvecs)
{
	size_t new_data_size = (size_t)(1 + nvecs) * sizeof(bitmapword);
	size_t new_entry_size = BMS_CHUNK_OVERHEAD + new_data_size;
	size_t total_used = bms_chunked_used_bytes(a);

	if (new_entry_size != old_entry_size)
	{
		/* May need to grow before shifting */
		if (new_entry_size > old_entry_size)
		{
			size_t extra = new_entry_size - old_entry_size;
			a = bms_chunked_ensure(a, total_used, extra);
		}

		uint8_t *buf = BMS_BUF(a);
		size_t tail_off = buf_off + old_entry_size;
		size_t tail_len = total_used - tail_off;

		if (tail_len > 0)
			memmove(buf + buf_off + new_entry_size, buf + tail_off, tail_len);
	}

	uint8_t *entry = BMS_BUF(a) + buf_off;
	bms_write_chunk_start(entry, start);
	bms_write_chunk_data(entry + BMS_CHUNK_OVERHEAD, desc, vecs, nvecs);

	return a;
}

/*
 * Insert a new chunk at buf_off. Shifts existing data to make room
 * and increments used_chunks.
 */
static inline Bitmapset *
bms_insert_chunk(Bitmapset *a, size_t buf_off,
				 uint64_t start, bitmapword desc,
				 bitmapword vecs[32], int nvecs)
{
	size_t new_data_size = (size_t)(1 + nvecs) * sizeof(bitmapword);
	size_t new_entry_size = BMS_CHUNK_OVERHEAD + new_data_size;
	size_t total_used = bms_chunked_used_bytes(a);

	a = bms_chunked_ensure(a, total_used, new_entry_size);

	uint8_t *buf = BMS_BUF(a);
	size_t tail_len = total_used - buf_off;

	if (tail_len > 0)
		memmove(buf + buf_off + new_entry_size, buf + buf_off, tail_len);

	uint8_t *entry = buf + buf_off;
	bms_write_chunk_start(entry, start);
	bms_write_chunk_data(entry + BMS_CHUNK_OVERHEAD, desc, vecs, nvecs);

	/* Increment used_chunks */
	unsigned alloc_chunks = BMS_ALLOC_CHUNKS(a);
	unsigned used = BMS_USED_CHUNKS(a) + 1;
	a->nwords = (alloc_chunks << 16) | used;

	return a;
}

/*
 * Remove a chunk at buf_off (of size entry_size) from the buffer.
 * Decrements used_chunks. Returns NULL if no chunks remain.
 */
static inline Bitmapset *
bms_remove_chunk(Bitmapset *a, size_t buf_off, size_t entry_size)
{
	size_t total_used = bms_chunked_used_bytes(a);
	uint8_t *buf = BMS_BUF(a);
	size_t tail_off = buf_off + entry_size;
	size_t tail_len = total_used - tail_off;

	if (tail_len > 0)
		memmove(buf + buf_off, buf + tail_off, tail_len);

	unsigned alloc_chunks = BMS_ALLOC_CHUNKS(a);
	unsigned used = BMS_USED_CHUNKS(a) - 1;

	if (used == 0)
	{
		pfree(a);
		return NULL;
	}

	a->nwords = (alloc_chunks << 16) | used;
	return a;
}

/*
 * Convert a dense bitmapset to chunked mode. Frees the dense bitmapset
 * and returns a new chunked one.
 */
static Bitmapset *
bms_dense_to_chunked(Bitmapset *dense)
{
	if (dense == NULL)
		return NULL;

	Assert(!BMS_IS_CHUNKED(dense));

	int nwords = BMS_NWORDS(dense);

	/*
	 * Scan the dense words and build chunk entries. Each chunk covers
	 * 32 words (2048 bits). Group consecutive 32-word blocks.
	 */

	/* First pass: count chunks needed */
	int nchunks = 0;
	for (int base = 0; base < nwords; base += 32)
	{
		int end = base + 32;
		if (end > nwords)
			end = nwords;
		bool has_bits = false;
		for (int w = base; w < end; w++)
		{
			if (BMS_WORDS(dense)[w] != 0)
			{
				has_bits = true;
				break;
			}
		}
		if (has_bits)
			nchunks++;
	}

	if (nchunks == 0)
	{
		pfree(dense);
		return NULL;
	}

	/* Compute initial allocation */
	/* Rough upper bound: each chunk needs at most BMS_CHUNK_OVERHEAD + (1 + 32) * 8 = 272 bytes */
	size_t max_bytes = (size_t)nchunks * (BMS_CHUNK_OVERHEAD + 33 * sizeof(bitmapword));
	unsigned alloc_chunks_units = (unsigned)((max_bytes + BMS_CHUNKED_ALLOC_UNIT - 1) / BMS_CHUNKED_ALLOC_UNIT);
	if (alloc_chunks_units < 1)
		alloc_chunks_units = 1;
	if (alloc_chunks_units > 0xFFFF)
		alloc_chunks_units = 0xFFFF;

	size_t alloc_sz = (size_t)alloc_chunks_units * BMS_CHUNKED_ALLOC_UNIT;
	Bitmapset *chunked = (Bitmapset *) palloc0(offsetof(Bitmapset, data) + alloc_sz);
	chunked->nwords = (alloc_chunks_units << 16) | 0;

	/* Second pass: encode chunks */
	uint8_t *p = BMS_BUF(chunked);
	unsigned used = 0;

	for (int base = 0; base < nwords; base += 32)
	{
		int end = base + 32;
		if (end > nwords)
			end = nwords;

		bitmapword words[32];
		int cap_flags[32];
		bool has_bits = false;

		for (int i = 0; i < 32; i++)
		{
			int w = base + i;
			if (w < end)
			{
				words[i] = BMS_WORDS(dense)[w];
				cap_flags[i] = 1;
				if (words[i] != 0)
					has_bits = true;
			}
			else
			{
				words[i] = 0;
				cap_flags[i] = 0;
			}
		}

		if (!has_bits)
			continue;

		bitmapword desc;
		bitmapword vecs[32];
		int nvecs;

		bms_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

		uint64_t start = (uint64_t)base * BITS_PER_BITMAPWORD;
		bms_write_chunk_start(p, start);
		p += BMS_CHUNK_OVERHEAD;
		p += bms_write_chunk_data(p, desc, vecs, nvecs);
		used++;
	}

	chunked->nwords = (alloc_chunks_units << 16) | used;
	pfree(dense);
	return chunked;
}

/*
 * Add a member to a chunked bitmapset using expand-modify-encode.
 */
static inline Bitmapset *
bms_chunked_add_member(Bitmapset *a, int64_t x)
{
	Assert(BMS_IS_CHUNKED(a));

	uint64_t aligned = bms_get_chunk_aligned_offset((size_t)x);
	size_t buf_off;
	int idx = bms_find_chunk(a, aligned, &buf_off);

	if (idx >= 0)
	{
		/* Chunk exists, expand-modify-encode */
		uint8_t *entry = BMS_BUF(a) + buf_off;
		size_t old_entry_size = bms_chunk_entry_bytes(entry);

		BmsChunk c;
		bms_init_chunk_at(entry, &c);

		bitmapword words[32];
		int cap_flags[32];
		bms_expand_chunk_words(&c, words, cap_flags);

		/* Set the bit */
		size_t within = (size_t)x - (size_t)aligned;
		int slot = (int)(within / BITS_PER_BITMAPWORD);
		int bit = (int)(within % BITS_PER_BITMAPWORD);

		/* Ensure slot has capacity */
		cap_flags[slot] = 1;
		words[slot] |= ((bitmapword)1 << bit);

		bitmapword desc;
		bitmapword vecs[32];
		int nvecs;
		bms_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

		a = bms_replace_chunk(a, buf_off, old_entry_size, aligned, desc, vecs, nvecs);
	}
	else
	{
		/* New chunk needed - encode a single-bit chunk */
		bitmapword words[32] = {0};
		int cap_flags[32] = {0};

		size_t within = (size_t)x - (size_t)aligned;
		int slot = (int)(within / BITS_PER_BITMAPWORD);
		int bit = (int)(within % BITS_PER_BITMAPWORD);

		cap_flags[slot] = 1;
		words[slot] = ((bitmapword)1 << bit);

		bitmapword desc;
		bitmapword vecs[32];
		int nvecs;
		bms_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

		a = bms_insert_chunk(a, buf_off, aligned, desc, vecs, nvecs);
	}

	return a;
}

/*
 * Helper: create an empty chunked bitmapset with given allocation.
 */
static inline Bitmapset *
bms_chunked_create(unsigned alloc_units)
{
	if (alloc_units < 1)
		alloc_units = 1;
	if (alloc_units > 0xFFFF)
		alloc_units = 0xFFFF;
	size_t alloc_sz = (size_t)alloc_units * BMS_CHUNKED_ALLOC_UNIT;
	Bitmapset *r = (Bitmapset *) palloc0(offsetof(Bitmapset, data) + alloc_sz);
	r->nwords = (alloc_units << 16) | 0;
	return r;
}

/*
 * Helper: append a chunk entry (start + encoded desc/vecs) to the end
 * of a chunked bitmapset's buffer, incrementing used_chunks.
 * Grows if needed; returns the (possibly reallocated) bitmapset.
 */
static inline Bitmapset *
bms_append_chunk(Bitmapset *r, size_t *used_bytes,
				 uint64_t start, bitmapword desc,
				 bitmapword vecs[32], int nvecs)
{
	size_t entry_sz = BMS_CHUNK_OVERHEAD + (size_t)(1 + nvecs) * sizeof(bitmapword);
	r = bms_chunked_ensure(r, *used_bytes, entry_sz);

	uint8_t *p = BMS_BUF(r) + *used_bytes;
	bms_write_chunk_start(p, start);
	bms_write_chunk_data(p + BMS_CHUNK_OVERHEAD, desc, vecs, nvecs);

	*used_bytes += entry_sz;

	unsigned alloc_chunks = BMS_ALLOC_CHUNKS(r);
	unsigned used = BMS_USED_CHUNKS(r) + 1;
	r->nwords = (alloc_chunks << 16) | used;

	return r;
}

/*
 * Helper: check if 32-word array has any non-zero word.
 */
static inline bool
bms_words_any_set(const bitmapword words[32])
{
	for (int i = 0; i < 32; i++)
		if (words[i] != 0)
			return true;
	return false;
}

/*
 * Helper: merge cap_flags via OR (union of capacities).
 */
static inline void
bms_cap_flags_or(int dst[32], const int a[32], const int b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] | b[i];
}

/* ----------------------------------------------------------------
 * Cross-mode promotion helpers
 *
 * When an operation receives a mix of dense and chunked operands,
 * the dense one must be promoted to chunked for the operation to
 * proceed.  These helpers factor out the promote/cleanup pattern.
 *
 * The promotion path (bms_ensure_chunked) allocates a temporary
 * copy and converts it to chunked mode.  This is intentional:
 * the inputs are const, so we cannot modify them in place.
 *
 * Mixed-mode operations are expected to be rare in practice --
 * most callers work with sets that are consistently dense or
 * consistently chunked.  If profiling shows this path is hot,
 * the caller should pre-convert operands to a uniform mode.
 * ---------------------------------------------------------------- */

/*
 * BmsPromoted: holds either the original (already-chunked) set or a
 * temporary copy-converted-to-chunked set.  Callers must call
 * bms_promoted_free() to release any temporary allocation.
 */
typedef struct BmsPromoted
{
	const Bitmapset *set;		/* promoted (or original) set */
	Bitmapset  *tmp;			/* non-NULL if we allocated a temp copy */
} BmsPromoted;

static inline BmsPromoted
bms_ensure_chunked(const Bitmapset *a)
{
	BmsPromoted p;

	if (BMS_IS_CHUNKED(a))
	{
		p.set = a;
		p.tmp = NULL;
	}
	else
	{
		p.tmp = bms_dense_to_chunked(bms_copy(a));
		p.set = p.tmp;
	}
	return p;
}

static inline void
bms_promoted_free(BmsPromoted *p)
{
	if (p->tmp)
		pfree(p->tmp);
}

/* ================================================================
 * Public API functions
 * ================================================================ */

/*
 * bms_copy - make a palloc'd copy of a bitmapset
 */
Bitmapset *
bms_copy(const Bitmapset *a)
{
	Bitmapset  *result;
	size_t		size;

	if (a == NULL)
		return NULL;

	if (BMS_IS_CHUNKED(a))
	{
		size_t alloc_sz = bms_chunked_alloc_size(a);
		size = offsetof(Bitmapset, data) + alloc_sz;
		result = (Bitmapset *) palloc(size);
		memcpy(result, a, offsetof(Bitmapset, data) + bms_chunked_used_bytes(a));
		result->nwords = a->nwords;
		return result;
	}

	size = BITMAPSET_SIZE(a->nwords);
	result = (Bitmapset *) palloc(size);
	memcpy(result, a, size);
	return result;
}

/*
 * bms_equal - are two bitmapsets equal? or both NULL?
 */
bool
bms_equal(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	if (a == NULL)
	{
		if (b == NULL)
			return true;
		return false;
	}
	else if (b == NULL)
		return false;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);
		unsigned used_a = BMS_USED_CHUNKS(pa.set);
		unsigned used_b = BMS_USED_CHUNKS(pb.set);
		bool equal = true;

		if (used_a != used_b)
		{
			equal = false;
		}
		else
		{
			const uint8_t *ba = BMS_BUF(pa.set);
			const uint8_t *bb = BMS_BUF(pb.set);

			for (unsigned ci = 0; ci < used_a; ci++)
			{
				if (bms_read_chunk_start(ba) != bms_read_chunk_start(bb))
				{
					equal = false;
					break;
				}

				/* Fast path: if raw encoded bytes are identical, skip expand */
				{
					size_t ea = bms_chunk_entry_bytes(ba);
					size_t eb = bms_chunk_entry_bytes(bb);
					if (ea == eb && memcmp(ba, bb, ea) == 0)
					{
						ba += ea;
						bb += eb;
						continue;
					}
				}

				BmsChunk cha, chb;

				bms_init_chunk_at(ba, &cha);
				bms_init_chunk_at(bb, &chb);

				bitmapword wa[32], wb[32];
				int cfa[32], cfb[32];

				bms_expand_chunk_words(&cha, wa, cfa);
				bms_expand_chunk_words(&chb, wb, cfb);

				for (int j = 0; j < 32; j++)
				{
					if (wa[j] != wb[j])
					{
						equal = false;
						break;
					}
				}
				if (!equal)
					break;

				ba += bms_chunk_entry_bytes(ba);
				bb += bms_chunk_entry_bytes(bb);
			}
		}

		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return equal;
	}

	/* Dense mode */
	if (a->nwords != b->nwords)
		return false;

	i = 0;
	do
	{
		if (BMS_WORDS(a)[i] != BMS_WORDS(b)[i])
			return false;
	} while (++i < BMS_NWORDS(a));

	return true;
}

/*
 * bms_compare - qsort-style comparator for bitmapsets
 */
int
bms_compare(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	if (a == NULL)
		return (b == NULL) ? 0 : -1;
	else if (b == NULL)
		return +1;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);
		unsigned used_a = BMS_USED_CHUNKS(pa.set);
		unsigned used_b = BMS_USED_CHUNKS(pb.set);
		const uint8_t *ba = BMS_BUF(pa.set);
		const uint8_t *bb = BMS_BUF(pb.set);
		unsigned ia = 0, ib = 0;
		int result = 0;

		/*
		 * Walk chunks from low addresses to high, overwriting `result`
		 * at each differing chunk.  This correctly reflects the most
		 * significant difference for qsort antisymmetry because:
		 *
		 * 1. Equal overlapping chunks leave `result` unchanged (the inner
		 *    j=31..0 loop only sets result when wa[j] != wb[j]).
		 * 2. Later iterations process higher-address chunks, which are
		 *    more significant.  The last-writer-wins semantics of the
		 *    outer loop ensure the highest chunk's result prevails.
		 * 3. Non-overlapping tails (after the main loop) correctly
		 *    set the final result for the highest remaining chunks.
		 */
		while (ia < used_a && ib < used_b)
		{
			uint64_t sa = bms_read_chunk_start(ba);
			uint64_t sb = bms_read_chunk_start(bb);

			if (sa < sb)
			{
				result = -1;
				ba += bms_chunk_entry_bytes(ba);
				ia++;
			}
			else if (sb < sa)
			{
				result = +1;
				bb += bms_chunk_entry_bytes(bb);
				ib++;
			}
			else
			{
				BmsChunk cha, chb;

				bms_init_chunk_at(ba, &cha);
				bms_init_chunk_at(bb, &chb);

				bitmapword wa[32], wb[32];
				int cfa[32], cfb[32];

				bms_expand_chunk_words(&cha, wa, cfa);
				bms_expand_chunk_words(&chb, wb, cfb);

				for (int j = 31; j >= 0; j--)
				{
					if (wa[j] != wb[j])
					{
						result = (wa[j] > wb[j]) ? +1 : -1;
						break;
					}
				}

				ba += bms_chunk_entry_bytes(ba);
				bb += bms_chunk_entry_bytes(bb);
				ia++;
				ib++;
			}
		}

		if (ia < used_a)
			result = +1;
		else if (ib < used_b)
			result = -1;

		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return result;
	}

	if (a->nwords != b->nwords)
		return (a->nwords > b->nwords) ? +1 : -1;

	i = BMS_NWORDS(a) - 1;
	do
	{
		bitmapword	aw = BMS_WORDS(a)[i];
		bitmapword	bw = BMS_WORDS(b)[i];

		if (aw != bw)
			return (aw > bw) ? +1 : -1;
	} while (--i >= 0);
	return 0;
}

/*
 * bms_make_singleton - build a bitmapset containing a single member
 */
Bitmapset *
bms_make_singleton(int64_t x)
{
	Bitmapset  *result;
	int64_t		wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");

	if (x > BMS_DENSE_MAX_BIT)
	{
		/* Create a chunked bitmapset with one chunk */
		unsigned alloc_chunks_units = 1;	/* 64 bytes is plenty for one chunk */
		size_t alloc_sz = (size_t)alloc_chunks_units * BMS_CHUNKED_ALLOC_UNIT;
		result = (Bitmapset *) palloc0(offsetof(Bitmapset, data) + alloc_sz);
		result->nwords = (alloc_chunks_units << 16) | 0;

		/* Add the single member */
		result = bms_chunked_add_member(result, x);
		return result;
	}

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	result = (Bitmapset *) palloc0(BITMAPSET_SIZE(wordnum + 1));
	result->nwords = (uint32_t)(wordnum + 1);
	BMS_WORDS(result)[wordnum] = ((bitmapword) 1 << bitnum);
	return result;
}

/*
 * bms_free - free a bitmapset
 */
void
bms_free(Bitmapset *a)
{
	if (a)
		pfree(a);
}

/* ----------------------------------------------------------------
 * Chunked-mode merge helper
 *
 * Five chunked operations (union, intersect, difference, overlap,
 * nonempty_difference) share a nearly identical two-pointer merge
 * structure.  This helper factors out the common walk.
 *
 * For set-producing ops (OR/AND/ANDNOT), returns a new Bitmapset
 * and ignores result_flag.
 *
 * For boolean ops (OVERLAP/NONEMPTY_DIFF), returns NULL and sets
 * *result_flag.  These ops early-exit on the first hit.
 *
 * used_bytes_out, if non-NULL, receives the total used bytes of
 * the result (avoids a redundant bms_chunked_used_bytes walk).
 * ---------------------------------------------------------------- */

typedef enum
{
	BMS_MERGE_OR,
	BMS_MERGE_AND,
	BMS_MERGE_ANDNOT,
	BMS_MERGE_OVERLAP,
	BMS_MERGE_NONEMPTY_DIFF,
} BmsMergeOp;

static Bitmapset *
bms_chunked_merge(const Bitmapset *a_chunked, const Bitmapset *b_chunked,
				  BmsMergeOp op, bool *result_flag, size_t *used_bytes_out)
{
	unsigned used_a = BMS_USED_CHUNKS(a_chunked);
	unsigned used_b = BMS_USED_CHUNKS(b_chunked);
	const uint8_t *p_a = BMS_BUF(a_chunked);
	const uint8_t *p_b = BMS_BUF(b_chunked);
	unsigned ia = 0, ib = 0;

	bool		is_set_op = (op == BMS_MERGE_OR || op == BMS_MERGE_AND ||
							 op == BMS_MERGE_ANDNOT);
	bool		bool_result = false;

	/* For set-producing ops, allocate result */
	Bitmapset  *r = NULL;
	size_t		used_bytes = 0;

	if (is_set_op)
	{
		size_t est;

		if (op == BMS_MERGE_OR)
			est = bms_chunked_used_bytes(a_chunked) +
				  bms_chunked_used_bytes(b_chunked);
		else
		{
			est = bms_chunked_used_bytes(a_chunked);
			size_t est_b = bms_chunked_used_bytes(b_chunked);
			if (op == BMS_MERGE_AND && est_b < est)
				est = est_b;
		}

		unsigned alloc_units = (unsigned)((est + BMS_CHUNKED_ALLOC_UNIT - 1) /
										  BMS_CHUNKED_ALLOC_UNIT);
		if (alloc_units < 1)
			alloc_units = 1;
		r = bms_chunked_create(alloc_units);
	}

	while (ia < used_a && ib < used_b)
	{
		uint64_t sa = bms_read_chunk_start(p_a);
		uint64_t sb = bms_read_chunk_start(p_b);

		if (sa < sb)
		{
			/* a-only chunk */
			switch (op)
			{
				case BMS_MERGE_OR:
				case BMS_MERGE_ANDNOT:
				{
					/* Keep a's chunk */
					BmsChunk c;
					bms_init_chunk_at(p_a, &c);
					bitmapword wa[32];
					int cfa[32];
					bms_expand_chunk_words(&c, wa, cfa);
					bitmapword desc;
					bitmapword vecs[32];
					int nvecs;
					bms_encode_sparse_chunk(wa, cfa, &desc, vecs, &nvecs);
					r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
					break;
				}
				case BMS_MERGE_AND:
					/* a-only chunk: AND produces nothing */
					break;
				case BMS_MERGE_OVERLAP:
					/* a-only: no overlap possible */
					break;
				case BMS_MERGE_NONEMPTY_DIFF:
					/* a has bits not in b */
					bool_result = true;
					goto done;
			}
			p_a += bms_chunk_entry_bytes(p_a);
			ia++;
		}
		else if (sb < sa)
		{
			/* b-only chunk */
			switch (op)
			{
				case BMS_MERGE_OR:
				{
					/* Keep b's chunk */
					BmsChunk c;
					bms_init_chunk_at(p_b, &c);
					bitmapword wb[32];
					int cfb[32];
					bms_expand_chunk_words(&c, wb, cfb);
					bitmapword desc;
					bitmapword vecs[32];
					int nvecs;
					bms_encode_sparse_chunk(wb, cfb, &desc, vecs, &nvecs);
					r = bms_append_chunk(r, &used_bytes, sb, desc, vecs, nvecs);
					break;
				}
				case BMS_MERGE_AND:
				case BMS_MERGE_ANDNOT:
					/* b-only: AND/ANDNOT produces nothing from b */
					break;
				case BMS_MERGE_OVERLAP:
					/* b-only: no overlap possible */
					break;
				case BMS_MERGE_NONEMPTY_DIFF:
					/* b-only: no diff bits from a */
					break;
			}
			p_b += bms_chunk_entry_bytes(p_b);
			ib++;
		}
		else
		{
			/* Overlapping chunks: expand both and apply op */
			BmsChunk ca_c, cb_c;
			bms_init_chunk_at(p_a, &ca_c);
			bms_init_chunk_at(p_b, &cb_c);
			bitmapword wa[32], wb[32];
			int cfa[32], cfb[32];
			bms_expand_chunk_words(&ca_c, wa, cfa);
			bms_expand_chunk_words(&cb_c, wb, cfb);

			if (is_set_op)
			{
				bitmapword wr[32];
				int cfr[32];

				switch (op)
				{
					case BMS_MERGE_OR:
						bms_words_or(wr, wa, wb);
						bms_cap_flags_or(cfr, cfa, cfb);
						break;
					case BMS_MERGE_AND:
						bms_words_and(wr, wa, wb);
						bms_cap_flags_or(cfr, cfa, cfb);
						break;
					case BMS_MERGE_ANDNOT:
						bms_words_andnot(wr, wa, wb);
						bms_cap_flags_or(cfr, cfa, cfb);
						break;
					default:
						/* unreachable */
						memset(wr, 0, sizeof(wr));
						memset(cfr, 0, sizeof(cfr));
						break;
				}

				/* For AND/ANDNOT, only emit if result has bits */
				if (op == BMS_MERGE_OR || bms_words_any_set(wr))
				{
					bitmapword desc;
					bitmapword vecs[32];
					int nvecs;
					bms_encode_sparse_chunk(wr, cfr, &desc, vecs, &nvecs);
					r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
				}
			}
			else
			{
				/* Boolean ops: check word-by-word */
				for (int j = 0; j < 32; j++)
				{
					bitmapword test;

					if (op == BMS_MERGE_OVERLAP)
						test = wa[j] & wb[j];
					else /* BMS_MERGE_NONEMPTY_DIFF */
						test = wa[j] & ~wb[j];

					if (test != 0)
					{
						bool_result = true;
						goto done;
					}
				}
			}

			p_a += bms_chunk_entry_bytes(p_a);
			ia++;
			p_b += bms_chunk_entry_bytes(p_b);
			ib++;
		}
	}

	/* Handle remaining chunks */
	if (is_set_op && (op == BMS_MERGE_OR || op == BMS_MERGE_ANDNOT))
	{
		/* Copy remaining from a */
		while (ia < used_a)
		{
			uint64_t sa = bms_read_chunk_start(p_a);
			BmsChunk c;
			bms_init_chunk_at(p_a, &c);
			bitmapword wa[32];
			int cfa[32];
			bms_expand_chunk_words(&c, wa, cfa);
			bitmapword desc;
			bitmapword vecs[32];
			int nvecs;
			bms_encode_sparse_chunk(wa, cfa, &desc, vecs, &nvecs);
			r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
			p_a += bms_chunk_entry_bytes(p_a);
			ia++;
		}
	}

	if (is_set_op && op == BMS_MERGE_OR)
	{
		/* Copy remaining from b */
		while (ib < used_b)
		{
			uint64_t sb = bms_read_chunk_start(p_b);
			BmsChunk c;
			bms_init_chunk_at(p_b, &c);
			bitmapword wb[32];
			int cfb[32];
			bms_expand_chunk_words(&c, wb, cfb);
			bitmapword desc;
			bitmapword vecs[32];
			int nvecs;
			bms_encode_sparse_chunk(wb, cfb, &desc, vecs, &nvecs);
			r = bms_append_chunk(r, &used_bytes, sb, desc, vecs, nvecs);
			p_b += bms_chunk_entry_bytes(p_b);
			ib++;
		}
	}

	/* For nonempty_difference: remaining a-chunks mean diff exists */
	if (op == BMS_MERGE_NONEMPTY_DIFF && ia < used_a)
		bool_result = true;

done:
	if (used_bytes_out)
		*used_bytes_out = used_bytes;

	if (!is_set_op)
	{
		*result_flag = bool_result;
		return NULL;
	}

	if (BMS_USED_CHUNKS(r) == 0)
	{
		pfree(r);
		return NULL;
	}
	return r;
}

/*
 * bms_union - set union, both inputs unmodified
 */
Bitmapset *
bms_union(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return bms_copy(a);

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);
		Bitmapset *r = bms_chunked_merge(pa.set, pb.set,
										  BMS_MERGE_OR, NULL, NULL);
		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return r;
	}

	if (BMS_NWORDS(a) <= BMS_NWORDS(b))
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = bms_copy(a);
		other = b;
	}
	otherlen = BMS_NWORDS(other);
	i = 0;
	do
	{
		BMS_WORDS(result)[i] |= BMS_WORDS(other)[i];
	} while (++i < otherlen);
	return result;
}

/*
 * bms_intersect - set intersection, both inputs unmodified
 */
Bitmapset *
bms_intersect(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			lastnonzero;
	int			resultlen;
	int			i;

	if (a == NULL || b == NULL)
		return NULL;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);
		Bitmapset *r = bms_chunked_merge(pa.set, pb.set,
										  BMS_MERGE_AND, NULL, NULL);
		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return r;
	}

	if (BMS_NWORDS(a) <= BMS_NWORDS(b))
	{
		result = bms_copy(a);
		other = b;
	}
	else
	{
		result = bms_copy(b);
		other = a;
	}
	resultlen = BMS_NWORDS(result);
	lastnonzero = -1;
	i = 0;
	do
	{
		BMS_WORDS(result)[i] &= BMS_WORDS(other)[i];

		if (BMS_WORDS(result)[i] != 0)
			lastnonzero = i;
	} while (++i < resultlen);
	if (lastnonzero == -1)
	{
		pfree(result);
		return NULL;
	}

	result->nwords = (uint32_t)(lastnonzero + 1);
	return result;
}

/*
 * bms_difference - set difference (a \ b), both inputs unmodified
 */
Bitmapset *
bms_difference(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	int			i;

	if (a == NULL)
		return NULL;
	if (b == NULL)
		return bms_copy(a);

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);
		Bitmapset *r = bms_chunked_merge(pa.set, pb.set,
										  BMS_MERGE_ANDNOT, NULL, NULL);
		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return r;
	}

	/*
	 * An empty result is a very common case, so it's worth optimizing for
	 * that by checking inline.  This saves us a malloc/free cycle compared
	 * to checking after-the-fact.  We inline the dense check here rather
	 * than calling bms_nonempty_difference() so the compiler can keep this
	 * as a tight loop without the overhead of the chunked code paths in
	 * the full function.
	 */
	if (BMS_NWORDS(a) <= BMS_NWORDS(b))
	{
		bool		has_diff = false;

		i = 0;
		do
		{
			if ((BMS_WORDS(a)[i] & ~BMS_WORDS(b)[i]) != 0)
			{
				has_diff = true;
				break;
			}
		} while (++i < BMS_NWORDS(a));

		if (!has_diff)
			return NULL;
	}
	/* else: a has more words than b, so there must be a difference */

	/* Copy the left input */
	result = bms_copy(a);

	/* And remove b's bits from result */
	if (BMS_NWORDS(result) > BMS_NWORDS(b))
	{
		/*
		 * We'll never need to remove trailing zero words when 'a' has more
		 * words than 'b' since the early-out above guarantees a non-empty
		 * result and the additional words must be non-zero.
		 */
		i = 0;
		do
		{
			BMS_WORDS(result)[i] &= ~BMS_WORDS(b)[i];
		} while (++i < BMS_NWORDS(b));
	}
	else
	{
		int			lastnonzero = -1;

		/* we may need to remove trailing zero words from the result. */
		i = 0;
		do
		{
			BMS_WORDS(result)[i] &= ~BMS_WORDS(b)[i];

			/* remember the last non-zero word */
			if (BMS_WORDS(result)[i] != 0)
				lastnonzero = i;
		} while (++i < BMS_NWORDS(result));

		/* trim off trailing zero words */
		result->nwords = (uint32_t)(lastnonzero + 1);
	}

	Assert(BMS_NWORDS(result) != 0);

	return result;
}

/*
 * bms_offset_members - shift all members by offset
 */
Bitmapset *
bms_offset_members(const Bitmapset *a, int64_t offset)
{
	Bitmapset  *result;
	int64_t		offset_words;
	int64_t		offset_bits;
	int64_t		new_nwords;
	int64_t		old_nwords;
	int64_t		high_bit;
	int64_t		old_highest;
	int64_t		new_highest;

	if (a == NULL)
		return NULL;

	if (BMS_IS_CHUNKED(a))
	{
		/* Iterate all members, add offset, build new set */
		Bitmapset *r = NULL;
		int64_t m = -1;
		while ((m = bms_next_member(a, m)) >= 0)
		{
			int64_t new_m = m + offset;
			if (new_m < 0)
				continue;
			r = bms_add_member(r, new_m);
		}
		return r;
	}

	old_nwords = BMS_NWORDS(a);

	/*
	 * Reject offsets so negative that all members would shift below zero.
	 * This also guards against INT64_MIN, where negation would overflow.
	 */
	if (offset < 0 &&
		(uint64_t)(-1 - offset) >= (uint64_t)old_nwords * BITS_PER_BITMAPWORD)
		return NULL;

	offset_words = WORDNUM(offset);
	offset_bits = BITNUM(offset);
	high_bit = bmw_leftmost_one_pos(BMS_WORDS(a)[old_nwords - 1]);
	old_highest = (old_nwords - 1) * BITS_PER_BITMAPWORD + high_bit;

	/* Check for overflow before adding */
	if (offset > 0 && old_highest > INT64_MAX - offset)
		elog(ERROR, "bitmapset member index overflow");
	new_highest = old_highest + offset;
	if (new_highest < 0)
		return NULL;

	new_nwords = WORDNUM(new_highest) + 1;
	result = (Bitmapset *) palloc0(BITMAPSET_SIZE(new_nwords));
	result->nwords = (uint32_t)new_nwords;

	if (offset >= 0)
	{
		if (offset_bits == 0)
		{
			int64_t	i = 0;

			do
			{
				Assert(i + offset_words < new_nwords);
				BMS_WORDS(result)[i + offset_words] = BMS_WORDS(a)[i];
			} while (++i < old_nwords);
		}
		else
		{
			int64_t		carry_bits = BITS_PER_BITMAPWORD - offset_bits;
			bitmapword	prev_carry = 0;
			int64_t		i = 0;

			do
			{
				bitmapword	carry = (BMS_WORDS(a)[i] >> carry_bits);

				Assert(i + offset_words < new_nwords);
				BMS_WORDS(result)[i + offset_words] = (BMS_WORDS(a)[i] << offset_bits) | prev_carry;
				prev_carry = carry;
			} while (++i < old_nwords);
			BMS_WORDS(result)[new_nwords - 1] |= prev_carry;
		}
	}
	else
	{
		offset_words = 0 - offset_words;
		offset_bits = 0 - offset_bits;

		if (offset_bits == 0)
		{
			int64_t	i = 0;

			do
			{
				Assert(i + offset_words < old_nwords);
				BMS_WORDS(result)[i] = BMS_WORDS(a)[i + offset_words];
			} while (++i < new_nwords);
		}
		else
		{
			int64_t		carry_bits = BITS_PER_BITMAPWORD - offset_bits;
			bitmapword	prev_carry = 0;
			int64_t		i = new_nwords - 1;

			if (old_nwords > new_nwords + offset_words)
				prev_carry = (BMS_WORDS(a)[new_nwords + offset_words] << carry_bits);

			do
			{
				bitmapword	carry = (BMS_WORDS(a)[i + offset_words] << carry_bits);

				Assert(i + offset_words < old_nwords);

				BMS_WORDS(result)[i] = (BMS_WORDS(a)[i + offset_words] >> offset_bits) | prev_carry;
				prev_carry = carry;
			} while (--i >= 0);
		}
	}

	return result;
}

/*
 * bms_is_subset - is A a subset of B?
 */
bool
bms_is_subset(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	if (a == NULL)
		return true;
	if (b == NULL)
		return false;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);

		unsigned used_a = BMS_USED_CHUNKS(pa.set);
		unsigned used_b = BMS_USED_CHUNKS(pb.set);
		const uint8_t *p_a = BMS_BUF(pa.set);
		const uint8_t *p_b = BMS_BUF(pb.set);
		unsigned ia = 0, ib = 0;
		bool is_subset = true;

		while (ia < used_a && ib < used_b)
		{
			uint64_t sa = bms_read_chunk_start(p_a);
			uint64_t sb = bms_read_chunk_start(p_b);

			if (sa < sb)
			{
				/* a has a chunk not in b => not subset */
				is_subset = false;
				break;
			}
			else if (sb < sa)
			{
				p_b += bms_chunk_entry_bytes(p_b);
				ib++;
			}
			else
			{
				BmsChunk ca_c, cb_c;
				bms_init_chunk_at(p_a, &ca_c);
				bms_init_chunk_at(p_b, &cb_c);
				bitmapword wa[32], wb[32];
				int cfa[32], cfb[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);
				for (int j = 0; j < 32; j++)
				{
					if ((wa[j] & ~wb[j]) != 0)
					{
						is_subset = false;
						break;
					}
				}
				if (!is_subset)
					break;
				p_a += bms_chunk_entry_bytes(p_a);
				ia++;
				p_b += bms_chunk_entry_bytes(p_b);
				ib++;
			}
		}

		/* If a has remaining chunks, not subset */
		if (is_subset && ia < used_a)
			is_subset = false;

		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return is_subset;
	}

	if (BMS_NWORDS(a) > BMS_NWORDS(b))
		return false;

	i = 0;
	do
	{
		if ((BMS_WORDS(a)[i] & ~BMS_WORDS(b)[i]) != 0)
			return false;
	} while (++i < BMS_NWORDS(a));
	return true;
}

/*
 * bms_subset_compare - compare A and B for equality/subset relationships
 */
BMS_Comparison
bms_subset_compare(const Bitmapset *a, const Bitmapset *b)
{
	BMS_Comparison result;
	int			shortlen;
	int			i;

	if (a == NULL)
	{
		if (b == NULL)
			return BMS_EQUAL;
		return BMS_SUBSET1;
	}
	if (b == NULL)
		return BMS_SUBSET2;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);

		unsigned used_a = BMS_USED_CHUNKS(pa.set);
		unsigned used_b = BMS_USED_CHUNKS(pb.set);
		const uint8_t *p_a = BMS_BUF(pa.set);
		const uint8_t *p_b = BMS_BUF(pb.set);
		unsigned ia = 0, ib = 0;
		BMS_Comparison cmp = BMS_EQUAL;

		while (ia < used_a && ib < used_b)
		{
			uint64_t sa = bms_read_chunk_start(p_a);
			uint64_t sb = bms_read_chunk_start(p_b);

			if (sa < sb)
			{
				/* a has chunk not in b => a has extra bits */
				if (cmp == BMS_SUBSET1)
				{
					cmp = BMS_DIFFERENT;
					break;
				}
				cmp = BMS_SUBSET2;
				p_a += bms_chunk_entry_bytes(p_a);
				ia++;
			}
			else if (sb < sa)
			{
				/* b has chunk not in a => b has extra bits */
				if (cmp == BMS_SUBSET2)
				{
					cmp = BMS_DIFFERENT;
					break;
				}
				cmp = BMS_SUBSET1;
				p_b += bms_chunk_entry_bytes(p_b);
				ib++;
			}
			else
			{
				BmsChunk ca_c, cb_c;
				bms_init_chunk_at(p_a, &ca_c);
				bms_init_chunk_at(p_b, &cb_c);
				bitmapword wa[32], wb[32];
				int cfa[32], cfb[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);

				for (int j = 0; j < 32; j++)
				{
					if ((wa[j] & ~wb[j]) != 0)
					{
						if (cmp == BMS_SUBSET1)
						{
							cmp = BMS_DIFFERENT;
							break;
						}
						cmp = BMS_SUBSET2;
					}
					if ((wb[j] & ~wa[j]) != 0)
					{
						if (cmp == BMS_SUBSET2)
						{
							cmp = BMS_DIFFERENT;
							break;
						}
						cmp = BMS_SUBSET1;
					}
				}
				if (cmp == BMS_DIFFERENT)
					break;
				p_a += bms_chunk_entry_bytes(p_a);
				ia++;
				p_b += bms_chunk_entry_bytes(p_b);
				ib++;
			}
		}

		if (cmp != BMS_DIFFERENT)
		{
			if (ia < used_a)
			{
				if (cmp == BMS_SUBSET1)
					cmp = BMS_DIFFERENT;
				else
					cmp = BMS_SUBSET2;
			}
			if (ib < used_b)
			{
				if (cmp == BMS_SUBSET2)
					cmp = BMS_DIFFERENT;
				else
					cmp = BMS_SUBSET1;
			}
		}

		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return cmp;
	}

	result = BMS_EQUAL;
	shortlen = Min(BMS_NWORDS(a), BMS_NWORDS(b));
	i = 0;
	do
	{
		bitmapword	aword = BMS_WORDS(a)[i];
		bitmapword	bword = BMS_WORDS(b)[i];

		if ((aword & ~bword) != 0)
		{
			if (result == BMS_SUBSET1)
				return BMS_DIFFERENT;
			result = BMS_SUBSET2;
		}
		if ((bword & ~aword) != 0)
		{
			if (result == BMS_SUBSET2)
				return BMS_DIFFERENT;
			result = BMS_SUBSET1;
		}
	} while (++i < shortlen);
	if (BMS_NWORDS(a) > BMS_NWORDS(b))
	{
		if (result == BMS_SUBSET1)
			return BMS_DIFFERENT;
		return BMS_SUBSET2;
	}
	else if (BMS_NWORDS(a) < BMS_NWORDS(b))
	{
		if (result == BMS_SUBSET2)
			return BMS_DIFFERENT;
		return BMS_SUBSET1;
	}
	return result;
}

/*
 * bms_is_member - is X a member of A?
 */
bool
bms_is_member(int64_t x, const Bitmapset *a)
{
	int64_t		wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return false;

	if (BMS_IS_CHUNKED(a))
	{
		uint64_t aligned = bms_get_chunk_aligned_offset((size_t)x);
		size_t buf_off;
		int idx = bms_find_chunk(a, aligned, &buf_off);
		if (idx < 0)
			return false;

		BmsChunk c;
		bms_init_chunk_at(BMS_BUF(a) + buf_off, &c);
		size_t within = (size_t)x - (size_t)aligned;
		return bms_chunk_is_set(&c, within);
	}

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	if (wordnum >= BMS_NWORDS(a))
		return false;
	if ((BMS_WORDS(a)[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
		return true;
	return false;
}

/*
 * bms_member_index - determine 0-based index of member x
 */
int64_t
bms_member_index(const Bitmapset *a, int64_t x)
{
	int64_t		bitnum;
	int64_t		wordnum;
	int64_t		result = 0;
	bitmapword	mask;

	if (!bms_is_member(x, a))
		return -1;

	if (BMS_IS_CHUNKED(a))
	{
		uint64_t aligned = bms_get_chunk_aligned_offset((size_t)x);
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			uint64_t chunk_start = bms_read_chunk_start(p);

			BmsChunk c;
			bms_init_chunk_at(p, &c);
			bitmapword words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			if (chunk_start < aligned)
			{
				/* Count all bits in this chunk */
				for (int j = 0; j < 32; j++)
					result += bmw_popcount(words[j]);
			}
			else if (chunk_start == aligned)
			{
				/* Count bits up to x */
				size_t within = (size_t)x - (size_t)chunk_start;
				int slot = (int)(within / BITS_PER_BITMAPWORD);
				int bit = (int)(within % BITS_PER_BITMAPWORD);

				for (int j = 0; j < slot; j++)
					result += bmw_popcount(words[j]);

				bitmapword m = ((bitmapword)1 << bit) - 1;
				result += bmw_popcount(words[slot] & m);
				return result;
			}
			else
			{
				/* past x's chunk - shouldn't happen if x is a member */
				break;
			}

			p += bms_chunk_entry_bytes(p);
		}
		/* Should not reach here if x is a member */
		return -1;
	}

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	for (int64_t i = 0; i < wordnum; i++)
		result += bmw_popcount(BMS_WORDS(a)[i]);

	mask = ((bitmapword) 1 << bitnum) - 1;
	result += bmw_popcount(BMS_WORDS(a)[wordnum] & mask);

	return result;
}

/*
 * bms_overlap - do sets overlap?
 */
bool
bms_overlap(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	if (a == NULL || b == NULL)
		return false;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);
		bool found = false;
		bms_chunked_merge(pa.set, pb.set, BMS_MERGE_OVERLAP, &found, NULL);
		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return found;
	}

	shortlen = Min(BMS_NWORDS(a), BMS_NWORDS(b));
	i = 0;
	do
	{
		if ((BMS_WORDS(a)[i] & BMS_WORDS(b)[i]) != 0)
			return true;
	} while (++i < shortlen);
	return false;
}

/*
 * bms_nonempty_difference - do sets have a nonempty difference?
 */
bool
bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	if (a == NULL)
		return false;
	if (b == NULL)
		return true;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		BmsPromoted pa = bms_ensure_chunked(a);
		BmsPromoted pb = bms_ensure_chunked(b);
		bool found = false;
		bms_chunked_merge(pa.set, pb.set,
						  BMS_MERGE_NONEMPTY_DIFF, &found, NULL);
		bms_promoted_free(&pa);
		bms_promoted_free(&pb);
		return found;
	}

	if (BMS_NWORDS(a) > BMS_NWORDS(b))
		return true;
	i = 0;
	do
	{
		if ((BMS_WORDS(a)[i] & ~BMS_WORDS(b)[i]) != 0)
			return true;
	} while (++i < BMS_NWORDS(a));
	return false;
}

/*
 * bms_singleton_member - return the sole integer member of set
 */
int64_t
bms_singleton_member(const Bitmapset *a)
{
	int64_t		result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		elog(ERROR, "bitmapset is empty");

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			uint64_t chunk_start = bms_read_chunk_start(p);
			BmsChunk c;
			bms_init_chunk_at(p, &c);
			bitmapword words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			for (int j = 0; j < 32; j++)
			{
				bitmapword w = words[j];
				if (w != 0)
				{
					if (result >= 0 || HAS_MULTIPLE_ONES(w))
						elog(ERROR, "bitmapset has multiple members");
					result = (int64_t)chunk_start + (int64_t)j * BITS_PER_BITMAPWORD;
					result += bmw_rightmost_one_pos(w);
				}
			}

			p += bms_chunk_entry_bytes(p);
		}

		Assert(result >= 0);
		return result;
	}

	nwords = BMS_NWORDS(a);
	wordnum = 0;
	do
	{
		bitmapword	w = BMS_WORDS(a)[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				elog(ERROR, "bitmapset has multiple members");
			result = (int64_t)wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	} while (++wordnum < nwords);

	Assert(result >= 0);
	return result;
}

/*
 * bms_get_singleton_member - safe singleton check
 */
bool
bms_get_singleton_member(const Bitmapset *a, int64_t *member)
{
	int64_t		result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return false;

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			uint64_t chunk_start = bms_read_chunk_start(p);
			BmsChunk c;
			bms_init_chunk_at(p, &c);
			bitmapword words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			for (int j = 0; j < 32; j++)
			{
				bitmapword w = words[j];
				if (w != 0)
				{
					if (result >= 0 || HAS_MULTIPLE_ONES(w))
						return false;
					result = (int64_t)chunk_start + (int64_t)j * BITS_PER_BITMAPWORD;
					result += bmw_rightmost_one_pos(w);
				}
			}

			p += bms_chunk_entry_bytes(p);
		}

		if (result < 0)
			return false;
		*member = result;
		return true;
	}

	nwords = BMS_NWORDS(a);
	wordnum = 0;
	do
	{
		bitmapword	w = BMS_WORDS(a)[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				return false;
			result = (int64_t)wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	} while (++wordnum < nwords);

	Assert(result >= 0);
	*member = result;
	return true;
}

/*
 * bms_num_members - count members of set
 */
int64_t
bms_num_members(const Bitmapset *a)
{
	int64_t		result = 0;
	int			nwords;
	int			i;

	if (a == NULL)
		return 0;

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			BmsChunk c;
			bms_init_chunk_at(p, &c);

			bitmapword words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			for (int j = 0; j < 32; j++)
				result += bmw_popcount(words[j]);

			p += bms_chunk_entry_bytes(p);
		}
		return result;
	}

	nwords = BMS_NWORDS(a);
	i = 0;
	do
	{
		result += bmw_popcount(BMS_WORDS(a)[i]);
	} while (++i < nwords);

	return result;
}

/*
 * bms_membership - empty/singleton/multiple?
 */
BMS_Membership
bms_membership(const Bitmapset *a)
{
	BMS_Membership result = BMS_EMPTY_SET;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return BMS_EMPTY_SET;

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);
		int found = 0;

		for (unsigned ci = 0; ci < used && found < 2; ci++)
		{
			BmsChunk c;
			bms_init_chunk_at(p, &c);

			bitmapword words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			for (int j = 0; j < 32 && found < 2; j++)
			{
				if (words[j] != 0)
				{
					found += bmw_popcount(words[j]);
				}
			}

			p += bms_chunk_entry_bytes(p);
		}

		if (found == 0)
			return BMS_EMPTY_SET;
		if (found == 1)
			return BMS_SINGLETON;
		return BMS_MULTIPLE;
	}

	nwords = BMS_NWORDS(a);
	wordnum = 0;
	do
	{
		bitmapword	w = BMS_WORDS(a)[wordnum];

		if (w != 0)
		{
			if (result != BMS_EMPTY_SET || HAS_MULTIPLE_ONES(w))
				return BMS_MULTIPLE;
			result = BMS_SINGLETON;
		}
	} while (++wordnum < nwords);
	return result;
}

/*
 * bms_add_member - add a specified member to set
 */
Bitmapset *
bms_add_member(Bitmapset *a, int64_t x)
{
	int64_t		wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return bms_make_singleton(x);

	if (BMS_IS_CHUNKED(a))
		return bms_chunked_add_member(a, x);

	/* If x would exceed dense capacity, convert to chunked first */
	if (x > BMS_DENSE_MAX_BIT)
	{
		a = bms_dense_to_chunked(a);
		return bms_chunked_add_member(a, x);
	}

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* Enlarge if necessary */
	if (wordnum >= BMS_NWORDS(a))
	{
		int			oldnwords = BMS_NWORDS(a);
		int64_t		i;

		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(wordnum + 1));
		a->nwords = (uint32_t)(wordnum + 1);
		/* zero out the enlarged portion */
		i = oldnwords;
		do
		{
			BMS_WORDS(a)[i] = 0;
		} while (++i < BMS_NWORDS(a));
	}

	BMS_WORDS(a)[wordnum] |= ((bitmapword) 1 << bitnum);

	return a;
}

/*
 * bms_del_member - remove a specified member from set
 */
Bitmapset *
bms_del_member(Bitmapset *a, int64_t x)
{
	int64_t		wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return NULL;

	if (BMS_IS_CHUNKED(a))
	{
		uint64_t aligned = bms_get_chunk_aligned_offset((size_t)x);
		size_t buf_off;
		int idx = bms_find_chunk(a, aligned, &buf_off);
		if (idx < 0)
			return a;	/* bit not in any chunk */

		uint8_t *entry = BMS_BUF(a) + buf_off;
		size_t old_entry_size = bms_chunk_entry_bytes(entry);

		BmsChunk c;
		bms_init_chunk_at(entry, &c);

		bitmapword words[32];
		int cap_flags[32];
		bms_expand_chunk_words(&c, words, cap_flags);

		/* Clear the bit */
		size_t within = (size_t)x - (size_t)aligned;
		int slot = (int)(within / BITS_PER_BITMAPWORD);
		int bit = (int)(within % BITS_PER_BITMAPWORD);
		words[slot] &= ~((bitmapword)1 << bit);

		/* Check if chunk is now empty */
		bitmapword desc;
		bitmapword vecs[32];
		int nvecs;
		bool has_bits = bms_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

		if (!has_bits)
			return bms_remove_chunk(a, buf_off, old_entry_size);

		return bms_replace_chunk(a, buf_off, old_entry_size, aligned, desc, vecs, nvecs);
	}

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	if (unlikely(wordnum >= BMS_NWORDS(a)))
		return a;

	BMS_WORDS(a)[wordnum] &= ~((bitmapword) 1 << bitnum);

	/* Trim trailing zero words */
	if (BMS_WORDS(a)[wordnum] == 0 && wordnum == BMS_NWORDS(a) - 1)
	{
		for (int64_t i = wordnum - 1; i >= 0; i--)
		{
			if (BMS_WORDS(a)[i] != 0)
			{
				a->nwords = (uint32_t)(i + 1);
				return a;
			}
		}

		pfree(a);
		return NULL;
	}
	return a;
}

/*
 * bms_add_members - like bms_union, but left input is recycled
 */
Bitmapset *
bms_add_members(Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return a;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		Bitmapset *u = bms_union(a, b);
		pfree(a);
		return u;
	}

	if (BMS_NWORDS(a) < BMS_NWORDS(b))
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	otherlen = BMS_NWORDS(other);
	i = 0;
	do
	{
		BMS_WORDS(result)[i] |= BMS_WORDS(other)[i];
	} while (++i < otherlen);
	if (result != a)
		pfree(a);

	return result;
}

/*
 * bms_replace_members - replace all members of a with members of b
 */
Bitmapset *
bms_replace_members(Bitmapset *a, const Bitmapset *b)
{
	int			i;

	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
	{
		pfree(a);
		return NULL;
	}

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		pfree(a);
		return bms_copy(b);
	}

	if (BMS_NWORDS(a) < BMS_NWORDS(b))
		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(BMS_NWORDS(b)));

	i = 0;
	do
	{
		BMS_WORDS(a)[i] = BMS_WORDS(b)[i];
	} while (++i < BMS_NWORDS(b));

	a->nwords = b->nwords;

	return a;
}

/*
 * bms_add_range - add members in range [lower, upper]
 */
Bitmapset *
bms_add_range(Bitmapset *a, int64_t lower, int64_t upper)
{
	int64_t		lwordnum,
				lbitnum,
				uwordnum,
				ushiftbits,
				wordnum;

	if (upper < lower)
		return a;

	if (lower < 0)
		elog(ERROR, "negative bitmapset member not allowed");

	/* If already chunked or range exceeds dense capacity, use chunked path */
	if ((a != NULL && BMS_IS_CHUNKED(a)) || upper > BMS_DENSE_MAX_BIT)
	{
		if (a != NULL && !BMS_IS_CHUNKED(a))
			a = bms_dense_to_chunked(a);

		if (a == NULL)
		{
			/* Create an empty chunked bitmapset */
			unsigned alloc_chunks_units = 1;
			size_t alloc_sz = (size_t)alloc_chunks_units * BMS_CHUNKED_ALLOC_UNIT;
			a = (Bitmapset *) palloc0(offsetof(Bitmapset, data) + alloc_sz);
			a->nwords = (alloc_chunks_units << 16) | 0;
		}

		/*
		 * Iterate over chunk-aligned regions in the range. For each chunk
		 * that overlaps [lower, upper], expand, set the bits, encode, replace/insert.
		 */
		uint64_t first_chunk = bms_get_chunk_aligned_offset((size_t)lower);
		uint64_t last_chunk = bms_get_chunk_aligned_offset((size_t)upper);

		/* Track scan position to avoid O(n^2) re-scanning from the start */
		size_t scan_off = 0;
		unsigned scan_idx = 0;

		for (uint64_t chunk_start = first_chunk; chunk_start <= last_chunk;
			 chunk_start += BMS_CHUNK_MAX_CAPACITY)
		{
			size_t buf_off = scan_off;
			int idx = bms_find_chunk_from(a, chunk_start, &buf_off, scan_idx);

			bitmapword words[32];
			int cap_flags[32];
			size_t old_entry_size = 0;

			if (idx >= 0)
			{
				uint8_t *entry = BMS_BUF(a) + buf_off;
				old_entry_size = bms_chunk_entry_bytes(entry);
				BmsChunk c;
				bms_init_chunk_at(entry, &c);
				bms_expand_chunk_words(&c, words, cap_flags);
			}
			else
			{
				memset(words, 0, sizeof(words));
				memset(cap_flags, 0, sizeof(cap_flags));
			}

			/* Set bits in range [lower, upper] within this chunk */
			int64_t range_lo = (int64_t)chunk_start;
			int64_t range_hi = (int64_t)chunk_start + BMS_CHUNK_MAX_CAPACITY - 1;
			if (range_lo < lower)
				range_lo = lower;
			if (range_hi > upper)
				range_hi = upper;

			/* Set bits word by word within this chunk */
			for (int64_t bit = range_lo; bit <= range_hi; )
			{
				size_t within = (size_t)(bit - (int64_t)chunk_start);
				int slot = (int)(within / BITS_PER_BITMAPWORD);
				int bit_in_slot = (int)(within % BITS_PER_BITMAPWORD);

				cap_flags[slot] = 1;

				/* How many bits can we set in this slot? */
				int64_t slot_end = (int64_t)chunk_start + (int64_t)(slot + 1) * BITS_PER_BITMAPWORD - 1;
				if (slot_end > range_hi)
					slot_end = range_hi;
				int bit_end_in_slot = (int)((size_t)(slot_end - (int64_t)chunk_start) % BITS_PER_BITMAPWORD);

				if (bit_in_slot == 0 && bit_end_in_slot == BITS_PER_BITMAPWORD - 1)
				{
					/* Full word */
					words[slot] = ~(uint64_t)0;
				}
				else
				{
					/* Partial word - create mask */
					bitmapword lo_mask = ~(bitmapword)(((bitmapword)1 << bit_in_slot) - 1);
					int ush = BITS_PER_BITMAPWORD - (bit_end_in_slot + 1);
					bitmapword hi_mask = (~(bitmapword)0) >> ush;
					words[slot] |= lo_mask & hi_mask;
				}

				bit = slot_end + 1;
			}

			bitmapword desc;
			bitmapword vecs[32];
			int nvecs;
			bms_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

			if (idx >= 0)
			{
				a = bms_replace_chunk(a, buf_off, old_entry_size, chunk_start, desc, vecs, nvecs);
				/* Advance past this entry for the next iteration */
				scan_off = buf_off + bms_chunk_entry_bytes(BMS_BUF(a) + buf_off);
				scan_idx = (unsigned)idx + 1;
			}
			else
			{
				/* idx encodes the insertion index as -(index + 1) */
				unsigned insert_idx = (unsigned)(-(idx + 1));

				a = bms_insert_chunk(a, buf_off, chunk_start, desc, vecs, nvecs);
				/* Advance past the newly inserted entry */
				scan_off = buf_off + bms_chunk_entry_bytes(BMS_BUF(a) + buf_off);
				scan_idx = insert_idx + 1;
			}
		}

		return a;
	}

	uwordnum = WORDNUM(upper);

	if (a == NULL)
	{
		a = (Bitmapset *) palloc0(BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = (uint32_t)(uwordnum + 1);
	}
	else if (uwordnum >= BMS_NWORDS(a))
	{
		int			oldnwords = BMS_NWORDS(a);
		int64_t		i;

		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = (uint32_t)(uwordnum + 1);
		i = oldnwords;
		do
		{
			BMS_WORDS(a)[i] = 0;
		} while (++i < BMS_NWORDS(a));
	}

	wordnum = lwordnum = WORDNUM(lower);

	lbitnum = BITNUM(lower);
	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(upper) + 1);

	if (lwordnum == uwordnum)
	{
		BMS_WORDS(a)[lwordnum] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1)
			& (~(bitmapword) 0) >> ushiftbits;
	}
	else
	{
		BMS_WORDS(a)[wordnum++] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1);

		while (wordnum < uwordnum)
			BMS_WORDS(a)[wordnum++] = ~(bitmapword) 0;

		BMS_WORDS(a)[uwordnum] |= (~(bitmapword) 0) >> ushiftbits;
	}

	return a;
}

/*
 * bms_int_members - intersect, recycling left input
 */
Bitmapset *
bms_int_members(Bitmapset *a, const Bitmapset *b)
{
	int			lastnonzero;
	int			shortlen;
	int			i;

	if (a == NULL)
		return NULL;
	if (b == NULL)
	{
		pfree(a);
		return NULL;
	}

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		Bitmapset *r = bms_intersect(a, b);
		pfree(a);
		return r;
	}

	shortlen = Min(BMS_NWORDS(a), BMS_NWORDS(b));
	lastnonzero = -1;
	i = 0;
	do
	{
		BMS_WORDS(a)[i] &= BMS_WORDS(b)[i];

		if (BMS_WORDS(a)[i] != 0)
			lastnonzero = i;
	} while (++i < shortlen);

	if (lastnonzero == -1)
	{
		pfree(a);
		return NULL;
	}

	a->nwords = (uint32_t)(lastnonzero + 1);

	return a;
}

/*
 * bms_del_members - remove members of b from a, recycling a
 */
Bitmapset *
bms_del_members(Bitmapset *a, const Bitmapset *b)
{
	int			i;

	if (a == NULL)
		return NULL;
	if (b == NULL)
		return a;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		Bitmapset *r = bms_difference(a, b);
		pfree(a);
		return r;
	}

	if (BMS_NWORDS(a) > BMS_NWORDS(b))
	{
		i = 0;
		do
		{
			BMS_WORDS(a)[i] &= ~BMS_WORDS(b)[i];
		} while (++i < BMS_NWORDS(b));
	}
	else
	{
		int			lastnonzero = -1;

		i = 0;
		do
		{
			BMS_WORDS(a)[i] &= ~BMS_WORDS(b)[i];

			if (BMS_WORDS(a)[i] != 0)
				lastnonzero = i;
		} while (++i < BMS_NWORDS(a));

		if (lastnonzero == -1)
		{
			pfree(a);
			return NULL;
		}

		a->nwords = (uint32_t)(lastnonzero + 1);
	}

	return a;
}

/*
 * bms_join - like bms_union, but either input may be recycled
 */
Bitmapset *
bms_join(Bitmapset *a, Bitmapset *b)
{
	Bitmapset  *result;
	Bitmapset  *other;
	int			otherlen;
	int			i;

	if (a == NULL)
		return b;
	if (b == NULL)
		return a;

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		Bitmapset *u = bms_union(a, b);
		pfree(a);
		if (b != a)
			pfree(b);
		return u;
	}

	if (BMS_NWORDS(a) < BMS_NWORDS(b))
	{
		result = b;
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	otherlen = BMS_NWORDS(other);
	i = 0;
	do
	{
		BMS_WORDS(result)[i] |= BMS_WORDS(other)[i];
	} while (++i < otherlen);
	if (other != result)
		pfree(other);

	return result;
}

/*
 * bms_next_member - find next member after prevbit
 *
 * Returns -2 if no more members.
 */
int64_t
bms_next_member(const Bitmapset *a, int64_t prevbit)
{
	int			nwords;
	bitmapword	mask;

	if (a == NULL)
		return -2;

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);
		int64_t target = prevbit + 1;

		for (unsigned ci = 0; ci < used; ci++)
		{
			uint64_t chunk_start = bms_read_chunk_start(p);
			int64_t chunk_end = (int64_t)chunk_start + BMS_CHUNK_MAX_CAPACITY - 1;

			if (chunk_end < target)
			{
				p += bms_chunk_entry_bytes(p);
				continue;
			}

			BmsChunk c;
			bms_init_chunk_at(p, &c);

			bitmapword words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			/* Search within this chunk starting from target */
			int64_t search_from = target;
			if (search_from < (int64_t)chunk_start)
				search_from = (int64_t)chunk_start;

			size_t within = (size_t)(search_from - (int64_t)chunk_start);
			int start_slot = (int)(within / BITS_PER_BITMAPWORD);
			int start_bit = (int)(within % BITS_PER_BITMAPWORD);

			for (int slot = start_slot; slot < 32; slot++)
			{
				bitmapword w = words[slot];
				if (slot == start_slot)
					w &= (~(bitmapword)0) << start_bit;

				if (w != 0)
				{
					int64_t result = (int64_t)chunk_start + (int64_t)slot * BITS_PER_BITMAPWORD;
					result += bmw_rightmost_one_pos(w);
					return result;
				}
			}

			p += bms_chunk_entry_bytes(p);
		}
		return -2;
	}

	nwords = BMS_NWORDS(a);
	prevbit++;
	mask = (~(bitmapword) 0) << BITNUM(prevbit);
	for (int64_t wordnum = WORDNUM(prevbit); wordnum < nwords; wordnum++)
	{
		bitmapword	w = BMS_WORDS(a)[wordnum];

		w &= mask;

		if (w != 0)
		{
			int64_t		result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
			return result;
		}

		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * bms_prev_member - find prev member before prevbit
 *
 * Returns -2 if no more members.
 */
int64_t
bms_prev_member(const Bitmapset *a, int64_t prevbit)
{
	int64_t		ushiftbits;
	bitmapword	mask;

	if (a == NULL || prevbit == 0)
		return -2;

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		if (used == 0)
			return -2;

		int64_t target;

		if (prevbit == -1)
		{
			/*
			 * When prevbit == -1, we want the highest member. Set target to
			 * the last possible bit in the last chunk -- this is a tight upper
			 * bound without needing to expand the chunk to find the exact max.
			 */
			const uint8_t *p = BMS_BUF(a);

			for (unsigned ci = 0; ci < used - 1; ci++)
				p += bms_chunk_entry_bytes(p);

			uint64_t chunk_start = bms_read_chunk_start(p);

			target = (int64_t)chunk_start + BMS_CHUNK_MAX_CAPACITY - 1;
		}
		else
		{
			target = prevbit - 1;
			if (target < 0)
				return -2;
		}

		/*
		 * Walk chunks forward, keeping the highest bit <= target found
		 * in any chunk.  Since chunks are sorted by start, once a chunk's
		 * start exceeds target we can stop.  For each candidate chunk we
		 * scan slots in reverse to find the highest set bit <= target.
		 * The last chunk that yields a hit gives the final answer.
		 */
		const uint8_t *p = BMS_BUF(a);
		int64_t best = -2;

		for (unsigned ci = 0; ci < used; ci++)
		{
			uint64_t chunk_start = bms_read_chunk_start(p);

			if ((int64_t)chunk_start > target)
				break;

			BmsChunk c;
			bms_init_chunk_at(p, &c);

			bitmapword words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			size_t within_limit;

			if (target >= (int64_t)chunk_start + BMS_CHUNK_MAX_CAPACITY)
				within_limit = BMS_CHUNK_MAX_CAPACITY - 1;
			else
				within_limit = (size_t)(target - (int64_t)chunk_start);

			int end_slot = (int)(within_limit / BITS_PER_BITMAPWORD);
			int end_bit = (int)(within_limit % BITS_PER_BITMAPWORD);

			for (int slot = end_slot; slot >= 0; slot--)
			{
				bitmapword w = words[slot];

				if (slot == end_slot)
				{
					int shift = BITS_PER_BITMAPWORD - (end_bit + 1);

					w &= (~(bitmapword)0) >> shift;
				}

				if (w != 0)
				{
					best = (int64_t)chunk_start +
						   (int64_t)slot * BITS_PER_BITMAPWORD +
						   bmw_leftmost_one_pos(w);
					break;
				}
			}

			p += bms_chunk_entry_bytes(p);
		}
		return best;
	}

	Assert(prevbit <= (int64_t)BMS_NWORDS(a) * BITS_PER_BITMAPWORD);
	Assert(prevbit >= -1);

	if (prevbit == -1)
		prevbit = (int64_t)BMS_NWORDS(a) * BITS_PER_BITMAPWORD - 1;
	else
		prevbit--;

	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(prevbit) + 1);
	mask = (~(bitmapword) 0) >> ushiftbits;
	for (int64_t wordnum = WORDNUM(prevbit); wordnum >= 0; wordnum--)
	{
		bitmapword	w = BMS_WORDS(a)[wordnum];

		w &= mask;

		if (w != 0)
		{
			int64_t		result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_leftmost_one_pos(w);
			return result;
		}

		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * Hash a uint64_t value into an FNV-1a accumulator using canonical
 * (little-endian) byte order via shifts, so the result is identical
 * regardless of the host's native byte order.
 */
static inline void
fnv1a_hash_uint64(uint32_t *hash, uint64_t val)
{
	for (int i = 0; i < 8; i++)
	{
		*hash ^= (unsigned char)(val >> (i * 8));
		*hash *= 16777619u;
	}
}

/*
 * bms_hash_value - compute a hash key for a Bitmapset
 *
 * Uses FNV-1a over (absolute_word_index, word_value) pairs for each
 * non-zero word.  This produces the same hash regardless of whether the
 * set is stored in dense or chunked mode.  Byte extraction uses shifts
 * rather than pointer casts to ensure identical results on big-endian
 * and little-endian machines.
 */
uint32_t
bms_hash_value(const Bitmapset *a)
{
	uint32_t	hash = 2166136261u;

	if (a == NULL)
		return 0;

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			uint64_t chunk_start = bms_read_chunk_start(p);
			BmsChunk c;
			uint64_t base_word;

			bms_init_chunk_at(p, &c);

			bitmapword words[32];
			int cap_flags[32];

			bms_expand_chunk_words(&c, words, cap_flags);

			/* Hash non-zero words using absolute word index */
			base_word = chunk_start / BITS_PER_BITMAPWORD;
			for (int w = 0; w < 32; w++)
			{
				if (words[w] == 0)
					continue;

				fnv1a_hash_uint64(&hash, base_word + (unsigned)w);
				fnv1a_hash_uint64(&hash, words[w]);
			}

			p += bms_chunk_entry_bytes(p);
		}
		return hash;
	}

	/* Dense mode: same scheme - hash (word_index, word_value) for non-zero */
	for (int i = 0; i < BMS_NWORDS(a); i++)
	{
		if (BMS_WORDS(a)[i] == 0)
			continue;

		fnv1a_hash_uint64(&hash, (uint64_t)i);
		fnv1a_hash_uint64(&hash, BMS_WORDS(a)[i]);
	}
	return hash;
}
