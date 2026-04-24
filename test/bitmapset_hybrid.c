/*-------------------------------------------------------------------------
 *
 * bitmapset_hybrid.c
 *	  Hybrid Bitmapset: PostgreSQL Bitmapset API + Sparsemap storage
 *
 * Dense mode implementation: word-array storage with uint32_t nwords
 * header and uint8_t data[] flexible array.
 *
 * Chunked mode (Phase 4): core operations using sparsemap chunk encoding.
 * Set operations on chunked bitmapsets (Phases 5-6) to be added.
 *
 *-------------------------------------------------------------------------
 */
#include "bitmapset_hybrid.h"
#include "chunk_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
 * Chunked mode constants
 */
#define BMS_DENSE_MAX_NWORDS  4096
#define BMS_DENSE_MAX_BIT     ((int64_t)BMS_DENSE_MAX_NWORDS * BITS_PER_BITMAPWORD - 1)
#define BMS_CHUNKED_ALLOC_UNIT 64

/*
 * BMS_DENSE_MAX_NWORDS must be less than 65536 so that dense nwords values
 * never have bits set in the upper 16 bits, which would be misidentified
 * as chunked mode by BMS_IS_CHUNKED().
 */
_Static_assert(BMS_DENSE_MAX_NWORDS < 65536,
               "BMS_DENSE_MAX_NWORDS must be < 65536 for mode detection");

/*
 * Memory allocation helpers that abort on failure.
 */
static inline void *
bms_alloc(size_t size)
{
	void	   *p = malloc(size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

static inline void *
bms_alloc0(size_t size)
{
	void	   *p = calloc(1, size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

static inline void *
bms_realloc(void *ptr, size_t size)
{
	void	   *p = realloc(ptr, size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

/*
 * Isolate rightmost one-bit using two's complement trick.
 */
#define RIGHTMOST_ONE(x) ((signedbitmapword) (x) & -((signedbitmapword) (x)))

#define HAS_MULTIPLE_ONES(x)	((bitmapword) RIGHTMOST_ONE(x) != (x))

#ifndef Min
#define Min(a, b)	((a) < (b) ? (a) : (b))
#endif

#if defined(__GNUC__) || defined(__clang__)
#define unlikely(x)	__builtin_expect((x) != 0, 0)
#else
#define unlikely(x)	(x)
#endif

/* ----------------------------------------------------------------
 * Chunked mode helper functions
 * ---------------------------------------------------------------- */

/*
 * Read the start offset from a chunk entry in the buffer.
 * Each entry is: [start: 8 bytes][descriptor: 8 bytes][vectors: 0-32 * 8 bytes]
 */
static inline __sm_idx_t
bms_read_chunk_start(const uint8_t *p)
{
	__sm_idx_t start;
	memcpy(&start, p, sizeof(__sm_idx_t));
	return start;
}

/*
 * Write the start offset into a chunk entry in the buffer.
 */
static inline void
bms_write_chunk_start(uint8_t *p, __sm_idx_t start)
{
	memcpy(p, &start, sizeof(__sm_idx_t));
}

/*
 * Initialize a __sm_chunk_t to point at the descriptor within a chunk entry.
 * The descriptor starts at offset SM_SIZEOF_OVERHEAD (8) from the entry start.
 */
static inline void
bms_init_chunk_at(const uint8_t *p, __sm_chunk_t *c)
{
	c->m_data = (__sm_bitvec_t *)(p + SM_SIZEOF_OVERHEAD);
}

/*
 * Total bytes occupied by a chunk entry: start (8) + chunk data (desc + vecs).
 */
static inline size_t
bms_chunk_entry_bytes(const uint8_t *p)
{
	__sm_chunk_t c;
	bms_init_chunk_at(p, &c);
	return SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&c);
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
 */
static inline int
bms_find_chunk(const Bitmapset *a, __sm_idx_t aligned, size_t *buf_off)
{
	unsigned used = BMS_USED_CHUNKS(a);
	const uint8_t *base = BMS_BUF(a);
	const uint8_t *p = base;

	for (unsigned i = 0; i < used; i++)
	{
		__sm_idx_t start = bms_read_chunk_start(p);
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
	a = (Bitmapset *) bms_realloc(a, offsetof(Bitmapset, data) + new_alloc_chunks * BMS_CHUNKED_ALLOC_UNIT);
	a->nwords = (new_alloc_chunks << 16) | used;
	return a;
}

/*
 * Expand any chunk (RLE or sparse) to the 32-word representation.
 */
static inline void
bms_expand_chunk_words(const __sm_chunk_t *chunk, __sm_bitvec_t words[32], int cap_flags[32])
{
	if (__sm_chunk_is_rle(chunk))
	{
		size_t len = __sm_chunk_rle_get_length(chunk);
		size_t cap = __sm_chunk_rle_get_capacity(chunk);
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
		__sm_expand_sparse_chunk(chunk, words, cap_flags);
	}
}

/*
 * Write encoded chunk data (descriptor + vectors) into buffer at p.
 * Returns the number of bytes written.
 */
static inline size_t
bms_write_chunk_data(uint8_t *p, __sm_bitvec_t desc, __sm_bitvec_t vecs[32], int nvecs)
{
	__sm_bitvec_t *dp = (__sm_bitvec_t *)p;
	dp[0] = desc;
	for (int i = 0; i < nvecs; i++)
		dp[1 + i] = vecs[i];
	return (size_t)(1 + nvecs) * sizeof(__sm_bitvec_t);
}

/*
 * Replace a chunk in-place in the buffer. Handles resizing if the new entry
 * is a different size than the old one.
 */
static inline Bitmapset *
bms_replace_chunk(Bitmapset *a, size_t buf_off, size_t old_entry_size,
				  __sm_idx_t start, __sm_bitvec_t desc,
				  __sm_bitvec_t vecs[32], int nvecs)
{
	size_t new_data_size = (size_t)(1 + nvecs) * sizeof(__sm_bitvec_t);
	size_t new_entry_size = SM_SIZEOF_OVERHEAD + new_data_size;
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
	bms_write_chunk_data(entry + SM_SIZEOF_OVERHEAD, desc, vecs, nvecs);

	return a;
}

/*
 * Insert a new chunk at buf_off. Shifts existing data to make room
 * and increments used_chunks.
 */
static inline Bitmapset *
bms_insert_chunk(Bitmapset *a, size_t buf_off,
				 __sm_idx_t start, __sm_bitvec_t desc,
				 __sm_bitvec_t vecs[32], int nvecs)
{
	size_t new_data_size = (size_t)(1 + nvecs) * sizeof(__sm_bitvec_t);
	size_t new_entry_size = SM_SIZEOF_OVERHEAD + new_data_size;
	size_t total_used = bms_chunked_used_bytes(a);

	a = bms_chunked_ensure(a, total_used, new_entry_size);

	uint8_t *buf = BMS_BUF(a);
	size_t tail_len = total_used - buf_off;

	if (tail_len > 0)
		memmove(buf + buf_off + new_entry_size, buf + buf_off, tail_len);

	uint8_t *entry = buf + buf_off;
	bms_write_chunk_start(entry, start);
	bms_write_chunk_data(entry + SM_SIZEOF_OVERHEAD, desc, vecs, nvecs);

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
		free(a);
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

	assert(!BMS_IS_CHUNKED(dense));

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
		free(dense);
		return NULL;
	}

	/* Compute initial allocation */
	/* Rough upper bound: each chunk needs at most SM_SIZEOF_OVERHEAD + (1 + 32) * 8 = 272 bytes */
	size_t max_bytes = (size_t)nchunks * (SM_SIZEOF_OVERHEAD + 33 * sizeof(__sm_bitvec_t));
	unsigned alloc_chunks_units = (unsigned)((max_bytes + BMS_CHUNKED_ALLOC_UNIT - 1) / BMS_CHUNKED_ALLOC_UNIT);
	if (alloc_chunks_units < 1)
		alloc_chunks_units = 1;
	if (alloc_chunks_units > 0xFFFF)
		alloc_chunks_units = 0xFFFF;

	size_t alloc_sz = (size_t)alloc_chunks_units * BMS_CHUNKED_ALLOC_UNIT;
	Bitmapset *chunked = (Bitmapset *) bms_alloc0(offsetof(Bitmapset, data) + alloc_sz);
	chunked->nwords = (alloc_chunks_units << 16) | 0;

	/* Second pass: encode chunks */
	uint8_t *p = BMS_BUF(chunked);
	unsigned used = 0;

	for (int base = 0; base < nwords; base += 32)
	{
		int end = base + 32;
		if (end > nwords)
			end = nwords;

		__sm_bitvec_t words[32];
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

		__sm_bitvec_t desc;
		__sm_bitvec_t vecs[32];
		int nvecs;

		__sm_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

		__sm_idx_t start = (__sm_idx_t)base * BITS_PER_BITMAPWORD;
		bms_write_chunk_start(p, start);
		p += SM_SIZEOF_OVERHEAD;
		p += bms_write_chunk_data(p, desc, vecs, nvecs);
		used++;
	}

	chunked->nwords = (alloc_chunks_units << 16) | used;
	free(dense);
	return chunked;
}

/*
 * Add a member to a chunked bitmapset using expand-modify-encode.
 */
static inline Bitmapset *
bms_chunked_add_member(Bitmapset *a, int64_t x)
{
	assert(BMS_IS_CHUNKED(a));

	__sm_idx_t aligned = __sm_get_chunk_aligned_offset((size_t)x);
	size_t buf_off;
	int idx = bms_find_chunk(a, aligned, &buf_off);

	if (idx >= 0)
	{
		/* Chunk exists, expand-modify-encode */
		uint8_t *entry = BMS_BUF(a) + buf_off;
		size_t old_entry_size = bms_chunk_entry_bytes(entry);

		__sm_chunk_t c;
		bms_init_chunk_at(entry, &c);

		__sm_bitvec_t words[32];
		int cap_flags[32];
		bms_expand_chunk_words(&c, words, cap_flags);

		/* Set the bit */
		size_t within = (size_t)x - (size_t)aligned;
		int slot = (int)(within / SM_BITS_PER_VECTOR);
		int bit = (int)(within % SM_BITS_PER_VECTOR);

		/* Ensure slot has capacity */
		cap_flags[slot] = 1;
		words[slot] |= ((__sm_bitvec_t)1 << bit);

		__sm_bitvec_t desc;
		__sm_bitvec_t vecs[32];
		int nvecs;
		__sm_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

		a = bms_replace_chunk(a, buf_off, old_entry_size, aligned, desc, vecs, nvecs);
	}
	else
	{
		/* New chunk needed - encode a single-bit chunk */
		__sm_bitvec_t words[32] = {0};
		int cap_flags[32] = {0};

		size_t within = (size_t)x - (size_t)aligned;
		int slot = (int)(within / SM_BITS_PER_VECTOR);
		int bit = (int)(within % SM_BITS_PER_VECTOR);

		cap_flags[slot] = 1;
		words[slot] = ((__sm_bitvec_t)1 << bit);

		__sm_bitvec_t desc;
		__sm_bitvec_t vecs[32];
		int nvecs;
		__sm_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

		a = bms_insert_chunk(a, buf_off, aligned, desc, vecs, nvecs);
	}

	return a;
}

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
		result = (Bitmapset *) bms_alloc(size);
		memcpy(result, a, offsetof(Bitmapset, data) + bms_chunked_used_bytes(a));
		result->nwords = a->nwords;
		return result;
	}

	size = BITMAPSET_SIZE(a->nwords);
	result = (Bitmapset *) bms_alloc(size);
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
		/* Chunked mode: compare chunk by chunk using expand */
		if (BMS_IS_CHUNKED(a) != BMS_IS_CHUNKED(b))
			return false;	/* different modes */

		unsigned used_a = BMS_USED_CHUNKS(a);
		unsigned used_b = BMS_USED_CHUNKS(b);
		if (used_a != used_b)
			return false;

		const uint8_t *pa = BMS_BUF(a);
		const uint8_t *pb = BMS_BUF(b);

		for (unsigned ci = 0; ci < used_a; ci++)
		{
			if (bms_read_chunk_start(pa) != bms_read_chunk_start(pb))
				return false;

			__sm_chunk_t ca, cb;
			bms_init_chunk_at(pa, &ca);
			bms_init_chunk_at(pb, &cb);

			__sm_bitvec_t wa[32], wb[32];
			int cfa[32], cfb[32];
			bms_expand_chunk_words(&ca, wa, cfa);
			bms_expand_chunk_words(&cb, wb, cfb);

			for (int j = 0; j < 32; j++)
			{
				if (wa[j] != wb[j])
					return false;
			}

			pa += bms_chunk_entry_bytes(pa);
			pb += bms_chunk_entry_bytes(pb);
		}
		return true;
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
		/* If modes differ, chunked > dense */
		if (BMS_IS_CHUNKED(a) && !BMS_IS_CHUNKED(b))
			return +1;
		if (!BMS_IS_CHUNKED(a) && BMS_IS_CHUNKED(b))
			return -1;

		/* Both chunked: compare by walking chunks in reverse */
		unsigned used_a = BMS_USED_CHUNKS(a);
		unsigned used_b = BMS_USED_CHUNKS(b);
		if (used_a != used_b)
			return (used_a > used_b) ? +1 : -1;

		/* Compare used bytes (buffer content) */
		size_t bytes_a = bms_chunked_used_bytes(a);
		size_t bytes_b = bms_chunked_used_bytes(b);
		if (bytes_a != bytes_b)
			return (bytes_a > bytes_b) ? +1 : -1;

		int cmp = memcmp(BMS_BUF(a), BMS_BUF(b), bytes_a);
		return (cmp > 0) ? +1 : (cmp < 0) ? -1 : 0;
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
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}

	if (x > BMS_DENSE_MAX_BIT)
	{
		/* Create a chunked bitmapset with one chunk */
		unsigned alloc_chunks_units = 1;	/* 64 bytes is plenty for one chunk */
		size_t alloc_sz = (size_t)alloc_chunks_units * BMS_CHUNKED_ALLOC_UNIT;
		result = (Bitmapset *) bms_alloc0(offsetof(Bitmapset, data) + alloc_sz);
		result->nwords = (alloc_chunks_units << 16) | 0;

		/* Add the single member */
		result = bms_chunked_add_member(result, x);
		return result;
	}

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	result = (Bitmapset *) bms_alloc0(BITMAPSET_SIZE(wordnum + 1));
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
		free(a);
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
	Bitmapset *r = (Bitmapset *) bms_alloc0(offsetof(Bitmapset, data) + alloc_sz);
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
				 __sm_idx_t start, __sm_bitvec_t desc,
				 __sm_bitvec_t vecs[32], int nvecs)
{
	size_t entry_sz = SM_SIZEOF_OVERHEAD + (size_t)(1 + nvecs) * sizeof(__sm_bitvec_t);
	r = bms_chunked_ensure(r, *used_bytes, entry_sz);

	uint8_t *p = BMS_BUF(r) + *used_bytes;
	bms_write_chunk_start(p, start);
	bms_write_chunk_data(p + SM_SIZEOF_OVERHEAD, desc, vecs, nvecs);

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
bms_words_any_set(const __sm_bitvec_t words[32])
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
		/* Convert dense operand to chunked if needed */
		Bitmapset *tmp_a = NULL, *tmp_b = NULL;
		const Bitmapset *ca = a, *cb = b;
		if (!BMS_IS_CHUNKED(a))
		{
			tmp_a = bms_dense_to_chunked(bms_copy(a));
			ca = tmp_a;
		}
		if (!BMS_IS_CHUNKED(b))
		{
			tmp_b = bms_dense_to_chunked(bms_copy(b));
			cb = tmp_b;
		}

		/* Two-pointer merge with OR */
		unsigned used_a = BMS_USED_CHUNKS(ca);
		unsigned used_b = BMS_USED_CHUNKS(cb);
		size_t est = bms_chunked_used_bytes(ca) + bms_chunked_used_bytes(cb);
		unsigned alloc_units = (unsigned)((est + BMS_CHUNKED_ALLOC_UNIT - 1) / BMS_CHUNKED_ALLOC_UNIT);
		if (alloc_units < 1) alloc_units = 1;
		Bitmapset *r = bms_chunked_create(alloc_units);
		size_t used_bytes = 0;

		const uint8_t *pa = BMS_BUF(ca);
		const uint8_t *pb = BMS_BUF(cb);
		unsigned ia = 0, ib = 0;

		while (ia < used_a && ib < used_b)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_idx_t sb = bms_read_chunk_start(pb);

			if (sa < sb)
			{
				/* Copy chunk from a */
				__sm_chunk_t c; bms_init_chunk_at(pa, &c);
				__sm_bitvec_t wa[32]; int cfa[32];
				bms_expand_chunk_words(&c, wa, cfa);
				__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
				__sm_encode_sparse_chunk(wa, cfa, &desc, vecs, &nvecs);
				r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
				pa += bms_chunk_entry_bytes(pa); ia++;
			}
			else if (sb < sa)
			{
				/* Copy chunk from b */
				__sm_chunk_t c; bms_init_chunk_at(pb, &c);
				__sm_bitvec_t wb[32]; int cfb[32];
				bms_expand_chunk_words(&c, wb, cfb);
				__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
				__sm_encode_sparse_chunk(wb, cfb, &desc, vecs, &nvecs);
				r = bms_append_chunk(r, &used_bytes, sb, desc, vecs, nvecs);
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
			else
			{
				/* Overlapping: OR the words */
				__sm_chunk_t ca_c, cb_c;
				bms_init_chunk_at(pa, &ca_c);
				bms_init_chunk_at(pb, &cb_c);
				__sm_bitvec_t wa[32], wb[32], wr[32];
				int cfa[32], cfb[32], cfr[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);
				__sm_words_or(wr, wa, wb);
				bms_cap_flags_or(cfr, cfa, cfb);
				__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
				__sm_encode_sparse_chunk(wr, cfr, &desc, vecs, &nvecs);
				r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
				pa += bms_chunk_entry_bytes(pa); ia++;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
		}

		/* Copy remaining from a */
		while (ia < used_a)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_chunk_t c; bms_init_chunk_at(pa, &c);
			__sm_bitvec_t wa[32]; int cfa[32];
			bms_expand_chunk_words(&c, wa, cfa);
			__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
			__sm_encode_sparse_chunk(wa, cfa, &desc, vecs, &nvecs);
			r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
			pa += bms_chunk_entry_bytes(pa); ia++;
		}

		/* Copy remaining from b */
		while (ib < used_b)
		{
			__sm_idx_t sb = bms_read_chunk_start(pb);
			__sm_chunk_t c; bms_init_chunk_at(pb, &c);
			__sm_bitvec_t wb[32]; int cfb[32];
			bms_expand_chunk_words(&c, wb, cfb);
			__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
			__sm_encode_sparse_chunk(wb, cfb, &desc, vecs, &nvecs);
			r = bms_append_chunk(r, &used_bytes, sb, desc, vecs, nvecs);
			pb += bms_chunk_entry_bytes(pb); ib++;
		}

		if (tmp_a) free(tmp_a);
		if (tmp_b) free(tmp_b);

		if (BMS_USED_CHUNKS(r) == 0)
		{
			free(r);
			return NULL;
		}
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
		Bitmapset *tmp_a = NULL, *tmp_b = NULL;
		const Bitmapset *ca = a, *cb = b;
		if (!BMS_IS_CHUNKED(a))
		{
			tmp_a = bms_dense_to_chunked(bms_copy(a));
			ca = tmp_a;
		}
		if (!BMS_IS_CHUNKED(b))
		{
			tmp_b = bms_dense_to_chunked(bms_copy(b));
			cb = tmp_b;
		}

		unsigned used_a = BMS_USED_CHUNKS(ca);
		unsigned used_b = BMS_USED_CHUNKS(cb);
		size_t est = bms_chunked_used_bytes(ca);
		if (bms_chunked_used_bytes(cb) < est) est = bms_chunked_used_bytes(cb);
		unsigned alloc_units = (unsigned)((est + BMS_CHUNKED_ALLOC_UNIT - 1) / BMS_CHUNKED_ALLOC_UNIT);
		if (alloc_units < 1) alloc_units = 1;
		Bitmapset *r = bms_chunked_create(alloc_units);
		size_t used_bytes = 0;

		const uint8_t *pa = BMS_BUF(ca);
		const uint8_t *pb = BMS_BUF(cb);
		unsigned ia = 0, ib = 0;

		while (ia < used_a && ib < used_b)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_idx_t sb = bms_read_chunk_start(pb);

			if (sa < sb)
			{
				pa += bms_chunk_entry_bytes(pa); ia++;
			}
			else if (sb < sa)
			{
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
			else
			{
				__sm_chunk_t ca_c, cb_c;
				bms_init_chunk_at(pa, &ca_c);
				bms_init_chunk_at(pb, &cb_c);
				__sm_bitvec_t wa[32], wb[32], wr[32];
				int cfa[32], cfb[32], cfr[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);
				__sm_words_and(wr, wa, wb);
				bms_cap_flags_or(cfr, cfa, cfb);
				if (bms_words_any_set(wr))
				{
					__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
					__sm_encode_sparse_chunk(wr, cfr, &desc, vecs, &nvecs);
					r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
				}
				pa += bms_chunk_entry_bytes(pa); ia++;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
		}

		if (tmp_a) free(tmp_a);
		if (tmp_b) free(tmp_b);

		if (BMS_USED_CHUNKS(r) == 0)
		{
			free(r);
			return NULL;
		}
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
		free(result);
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
		if (!bms_nonempty_difference(a, b))
			return NULL;

		Bitmapset *tmp_a = NULL, *tmp_b = NULL;
		const Bitmapset *ca = a, *cb = b;
		if (!BMS_IS_CHUNKED(a))
		{
			tmp_a = bms_dense_to_chunked(bms_copy(a));
			ca = tmp_a;
		}
		if (!BMS_IS_CHUNKED(b))
		{
			tmp_b = bms_dense_to_chunked(bms_copy(b));
			cb = tmp_b;
		}

		unsigned used_a = BMS_USED_CHUNKS(ca);
		unsigned used_b = BMS_USED_CHUNKS(cb);
		size_t est = bms_chunked_used_bytes(ca);
		unsigned alloc_units = (unsigned)((est + BMS_CHUNKED_ALLOC_UNIT - 1) / BMS_CHUNKED_ALLOC_UNIT);
		if (alloc_units < 1) alloc_units = 1;
		Bitmapset *r = bms_chunked_create(alloc_units);
		size_t used_bytes = 0;

		const uint8_t *pa = BMS_BUF(ca);
		const uint8_t *pb = BMS_BUF(cb);
		unsigned ia = 0, ib = 0;

		while (ia < used_a && ib < used_b)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_idx_t sb = bms_read_chunk_start(pb);

			if (sa < sb)
			{
				/* a chunk not in b: keep as-is */
				__sm_chunk_t c; bms_init_chunk_at(pa, &c);
				__sm_bitvec_t wa[32]; int cfa[32];
				bms_expand_chunk_words(&c, wa, cfa);
				__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
				__sm_encode_sparse_chunk(wa, cfa, &desc, vecs, &nvecs);
				r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
				pa += bms_chunk_entry_bytes(pa); ia++;
			}
			else if (sb < sa)
			{
				/* b chunk not in a: skip */
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
			else
			{
				/* Overlapping: ANDNOT */
				__sm_chunk_t ca_c, cb_c;
				bms_init_chunk_at(pa, &ca_c);
				bms_init_chunk_at(pb, &cb_c);
				__sm_bitvec_t wa[32], wb[32], wr[32];
				int cfa[32], cfb[32], cfr[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);
				__sm_words_andnot(wr, wa, wb);
				bms_cap_flags_or(cfr, cfa, cfb);
				if (bms_words_any_set(wr))
				{
					__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
					__sm_encode_sparse_chunk(wr, cfr, &desc, vecs, &nvecs);
					r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
				}
				pa += bms_chunk_entry_bytes(pa); ia++;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
		}

		/* Copy remaining from a */
		while (ia < used_a)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_chunk_t c; bms_init_chunk_at(pa, &c);
			__sm_bitvec_t wa[32]; int cfa[32];
			bms_expand_chunk_words(&c, wa, cfa);
			__sm_bitvec_t desc; __sm_bitvec_t vecs[32]; int nvecs;
			__sm_encode_sparse_chunk(wa, cfa, &desc, vecs, &nvecs);
			r = bms_append_chunk(r, &used_bytes, sa, desc, vecs, nvecs);
			pa += bms_chunk_entry_bytes(pa); ia++;
		}

		if (tmp_a) free(tmp_a);
		if (tmp_b) free(tmp_b);

		if (BMS_USED_CHUNKS(r) == 0)
		{
			free(r);
			return NULL;
		}
		return r;
	}

	if (!bms_nonempty_difference(a, b))
		return NULL;

	result = bms_copy(a);

	if (BMS_NWORDS(result) > BMS_NWORDS(b))
	{
		i = 0;
		do
		{
			BMS_WORDS(result)[i] &= ~BMS_WORDS(b)[i];
		} while (++i < BMS_NWORDS(b));
	}
	else
	{
		int			lastnonzero = -1;

		i = 0;
		do
		{
			BMS_WORDS(result)[i] &= ~BMS_WORDS(b)[i];

			if (BMS_WORDS(result)[i] != 0)
				lastnonzero = i;
		} while (++i < BMS_NWORDS(result));

		result->nwords = (uint32_t)(lastnonzero + 1);
	}
	assert(BMS_NWORDS(result) != 0);

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
	offset_words = WORDNUM(offset);
	offset_bits = BITNUM(offset);
	high_bit = bmw_leftmost_one_pos(BMS_WORDS(a)[old_nwords - 1]);
	old_highest = (old_nwords - 1) * BITS_PER_BITMAPWORD + high_bit;

	new_highest = old_highest + offset;
	if (new_highest < 0)
		return NULL;

	new_nwords = WORDNUM(new_highest) + 1;
	result = (Bitmapset *) bms_alloc0(BITMAPSET_SIZE(new_nwords));
	result->nwords = (uint32_t)new_nwords;

	if (offset >= 0)
	{
		if (offset_bits == 0)
		{
			int64_t	i = 0;

			do
			{
				assert(i + offset_words < new_nwords);
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

				assert(i + offset_words < new_nwords);
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
				assert(i + offset_words < old_nwords);
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

				assert(i + offset_words < old_nwords);

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
		Bitmapset *tmp_a = NULL, *tmp_b = NULL;
		const Bitmapset *ca = a, *cb = b;
		if (!BMS_IS_CHUNKED(a))
		{
			tmp_a = bms_dense_to_chunked(bms_copy(a));
			ca = tmp_a;
		}
		if (!BMS_IS_CHUNKED(b))
		{
			tmp_b = bms_dense_to_chunked(bms_copy(b));
			cb = tmp_b;
		}

		unsigned used_a = BMS_USED_CHUNKS(ca);
		unsigned used_b = BMS_USED_CHUNKS(cb);
		const uint8_t *pa = BMS_BUF(ca);
		const uint8_t *pb = BMS_BUF(cb);
		unsigned ia = 0, ib = 0;
		bool is_subset = true;

		while (ia < used_a && ib < used_b)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_idx_t sb = bms_read_chunk_start(pb);

			if (sa < sb)
			{
				/* a has a chunk not in b => not subset */
				is_subset = false;
				break;
			}
			else if (sb < sa)
			{
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
			else
			{
				__sm_chunk_t ca_c, cb_c;
				bms_init_chunk_at(pa, &ca_c);
				bms_init_chunk_at(pb, &cb_c);
				__sm_bitvec_t wa[32], wb[32];
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
				if (!is_subset) break;
				pa += bms_chunk_entry_bytes(pa); ia++;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
		}

		/* If a has remaining chunks, not subset */
		if (is_subset && ia < used_a)
			is_subset = false;

		if (tmp_a) free(tmp_a);
		if (tmp_b) free(tmp_b);
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
		Bitmapset *tmp_a = NULL, *tmp_b = NULL;
		const Bitmapset *ca = a, *cb = b;
		if (!BMS_IS_CHUNKED(a))
		{
			tmp_a = bms_dense_to_chunked(bms_copy(a));
			ca = tmp_a;
		}
		if (!BMS_IS_CHUNKED(b))
		{
			tmp_b = bms_dense_to_chunked(bms_copy(b));
			cb = tmp_b;
		}

		unsigned used_a = BMS_USED_CHUNKS(ca);
		unsigned used_b = BMS_USED_CHUNKS(cb);
		const uint8_t *pa = BMS_BUF(ca);
		const uint8_t *pb = BMS_BUF(cb);
		unsigned ia = 0, ib = 0;
		BMS_Comparison cmp = BMS_EQUAL;

		while (ia < used_a && ib < used_b)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_idx_t sb = bms_read_chunk_start(pb);

			if (sa < sb)
			{
				/* a has chunk not in b => a has extra bits */
				if (cmp == BMS_SUBSET1) { cmp = BMS_DIFFERENT; break; }
				cmp = BMS_SUBSET2;
				pa += bms_chunk_entry_bytes(pa); ia++;
			}
			else if (sb < sa)
			{
				/* b has chunk not in a => b has extra bits */
				if (cmp == BMS_SUBSET2) { cmp = BMS_DIFFERENT; break; }
				cmp = BMS_SUBSET1;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
			else
			{
				__sm_chunk_t ca_c, cb_c;
				bms_init_chunk_at(pa, &ca_c);
				bms_init_chunk_at(pb, &cb_c);
				__sm_bitvec_t wa[32], wb[32];
				int cfa[32], cfb[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);

				for (int j = 0; j < 32; j++)
				{
					if ((wa[j] & ~wb[j]) != 0)
					{
						if (cmp == BMS_SUBSET1) { cmp = BMS_DIFFERENT; break; }
						cmp = BMS_SUBSET2;
					}
					if ((wb[j] & ~wa[j]) != 0)
					{
						if (cmp == BMS_SUBSET2) { cmp = BMS_DIFFERENT; break; }
						cmp = BMS_SUBSET1;
					}
				}
				if (cmp == BMS_DIFFERENT) break;
				pa += bms_chunk_entry_bytes(pa); ia++;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
		}

		if (cmp != BMS_DIFFERENT)
		{
			if (ia < used_a)
			{
				if (cmp == BMS_SUBSET1) cmp = BMS_DIFFERENT;
				else cmp = BMS_SUBSET2;
			}
			if (ib < used_b)
			{
				if (cmp == BMS_SUBSET2) cmp = BMS_DIFFERENT;
				else cmp = BMS_SUBSET1;
			}
		}

		if (tmp_a) free(tmp_a);
		if (tmp_b) free(tmp_b);
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
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
	if (a == NULL)
		return false;

	if (BMS_IS_CHUNKED(a))
	{
		__sm_idx_t aligned = __sm_get_chunk_aligned_offset((size_t)x);
		size_t buf_off;
		int idx = bms_find_chunk(a, aligned, &buf_off);
		if (idx < 0)
			return false;

		__sm_chunk_t c;
		bms_init_chunk_at(BMS_BUF(a) + buf_off, &c);
		size_t within = (size_t)x - (size_t)aligned;
		return __sm_chunk_is_set(&c, within);
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
bms_member_index(Bitmapset *a, int64_t x)
{
	int64_t		bitnum;
	int64_t		wordnum;
	int64_t		result = 0;
	bitmapword	mask;

	if (!bms_is_member(x, a))
		return -1;

	if (BMS_IS_CHUNKED(a))
	{
		__sm_idx_t aligned = __sm_get_chunk_aligned_offset((size_t)x);
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			__sm_idx_t chunk_start = bms_read_chunk_start(p);

			__sm_chunk_t c;
			bms_init_chunk_at(p, &c);
			__sm_bitvec_t words[32];
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
				int slot = (int)(within / SM_BITS_PER_VECTOR);
				int bit = (int)(within % SM_BITS_PER_VECTOR);

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
		Bitmapset *tmp_a = NULL, *tmp_b = NULL;
		const Bitmapset *ca = a, *cb = b;
		if (!BMS_IS_CHUNKED(a))
		{
			tmp_a = bms_dense_to_chunked(bms_copy(a));
			ca = tmp_a;
		}
		if (!BMS_IS_CHUNKED(b))
		{
			tmp_b = bms_dense_to_chunked(bms_copy(b));
			cb = tmp_b;
		}

		unsigned used_a = BMS_USED_CHUNKS(ca);
		unsigned used_b = BMS_USED_CHUNKS(cb);
		const uint8_t *pa = BMS_BUF(ca);
		const uint8_t *pb = BMS_BUF(cb);
		unsigned ia = 0, ib = 0;
		bool found = false;

		while (ia < used_a && ib < used_b)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_idx_t sb = bms_read_chunk_start(pb);

			if (sa < sb)
			{
				pa += bms_chunk_entry_bytes(pa); ia++;
			}
			else if (sb < sa)
			{
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
			else
			{
				__sm_chunk_t ca_c, cb_c;
				bms_init_chunk_at(pa, &ca_c);
				bms_init_chunk_at(pb, &cb_c);
				__sm_bitvec_t wa[32], wb[32];
				int cfa[32], cfb[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);

				for (int j = 0; j < 32; j++)
				{
					if ((wa[j] & wb[j]) != 0)
					{
						found = true;
						break;
					}
				}
				if (found) break;
				pa += bms_chunk_entry_bytes(pa); ia++;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
		}

		if (tmp_a) free(tmp_a);
		if (tmp_b) free(tmp_b);
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
		Bitmapset *tmp_a = NULL, *tmp_b = NULL;
		const Bitmapset *ca = a, *cb = b;
		if (!BMS_IS_CHUNKED(a))
		{
			tmp_a = bms_dense_to_chunked(bms_copy(a));
			ca = tmp_a;
		}
		if (!BMS_IS_CHUNKED(b))
		{
			tmp_b = bms_dense_to_chunked(bms_copy(b));
			cb = tmp_b;
		}

		unsigned used_a = BMS_USED_CHUNKS(ca);
		unsigned used_b = BMS_USED_CHUNKS(cb);
		const uint8_t *pa = BMS_BUF(ca);
		const uint8_t *pb = BMS_BUF(cb);
		unsigned ia = 0, ib = 0;
		bool found = false;

		while (ia < used_a && ib < used_b)
		{
			__sm_idx_t sa = bms_read_chunk_start(pa);
			__sm_idx_t sb = bms_read_chunk_start(pb);

			if (sa < sb)
			{
				/* a has chunk not in b => nonempty diff */
				found = true;
				break;
			}
			else if (sb < sa)
			{
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
			else
			{
				__sm_chunk_t ca_c, cb_c;
				bms_init_chunk_at(pa, &ca_c);
				bms_init_chunk_at(pb, &cb_c);
				__sm_bitvec_t wa[32], wb[32];
				int cfa[32], cfb[32];
				bms_expand_chunk_words(&ca_c, wa, cfa);
				bms_expand_chunk_words(&cb_c, wb, cfb);

				for (int j = 0; j < 32; j++)
				{
					if ((wa[j] & ~wb[j]) != 0)
					{
						found = true;
						break;
					}
				}
				if (found) break;
				pa += bms_chunk_entry_bytes(pa); ia++;
				pb += bms_chunk_entry_bytes(pb); ib++;
			}
		}

		/* If a has remaining chunks => nonempty diff */
		if (!found && ia < used_a)
			found = true;

		if (tmp_a) free(tmp_a);
		if (tmp_b) free(tmp_b);
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
	{
		fprintf(stderr, "bitmapset is empty\n");
		abort();
	}

	if (BMS_IS_CHUNKED(a))
	{
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			__sm_idx_t chunk_start = bms_read_chunk_start(p);
			__sm_chunk_t c;
			bms_init_chunk_at(p, &c);
			__sm_bitvec_t words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			for (int j = 0; j < 32; j++)
			{
				bitmapword w = words[j];
				if (w != 0)
				{
					if (result >= 0 || HAS_MULTIPLE_ONES(w))
					{
						fprintf(stderr, "bitmapset has multiple members\n");
						abort();
					}
					result = (int64_t)chunk_start + (int64_t)j * SM_BITS_PER_VECTOR;
					result += bmw_rightmost_one_pos(w);
				}
			}

			p += bms_chunk_entry_bytes(p);
		}

		assert(result >= 0);
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
			{
				fprintf(stderr, "bitmapset has multiple members\n");
				abort();
			}
			result = (int64_t)wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	} while (++wordnum < nwords);

	assert(result >= 0);
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
			__sm_idx_t chunk_start = bms_read_chunk_start(p);
			__sm_chunk_t c;
			bms_init_chunk_at(p, &c);
			__sm_bitvec_t words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			for (int j = 0; j < 32; j++)
			{
				bitmapword w = words[j];
				if (w != 0)
				{
					if (result >= 0 || HAS_MULTIPLE_ONES(w))
						return false;
					result = (int64_t)chunk_start + (int64_t)j * SM_BITS_PER_VECTOR;
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

	assert(result >= 0);
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
			__sm_chunk_t c;
			bms_init_chunk_at(p, &c);

			__sm_bitvec_t words[32];
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
			__sm_chunk_t c;
			bms_init_chunk_at(p, &c);

			__sm_bitvec_t words[32];
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
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
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

		a = (Bitmapset *) bms_realloc(a, BITMAPSET_SIZE(wordnum + 1));
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
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
	if (a == NULL)
		return NULL;

	if (BMS_IS_CHUNKED(a))
	{
		__sm_idx_t aligned = __sm_get_chunk_aligned_offset((size_t)x);
		size_t buf_off;
		int idx = bms_find_chunk(a, aligned, &buf_off);
		if (idx < 0)
			return a;	/* bit not in any chunk */

		uint8_t *entry = BMS_BUF(a) + buf_off;
		size_t old_entry_size = bms_chunk_entry_bytes(entry);

		__sm_chunk_t c;
		bms_init_chunk_at(entry, &c);

		__sm_bitvec_t words[32];
		int cap_flags[32];
		bms_expand_chunk_words(&c, words, cap_flags);

		/* Clear the bit */
		size_t within = (size_t)x - (size_t)aligned;
		int slot = (int)(within / SM_BITS_PER_VECTOR);
		int bit = (int)(within % SM_BITS_PER_VECTOR);
		words[slot] &= ~((__sm_bitvec_t)1 << bit);

		/* Check if chunk is now empty */
		__sm_bitvec_t desc;
		__sm_bitvec_t vecs[32];
		int nvecs;
		bool has_bits = __sm_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

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

		free(a);
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
		free(a);
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
		free(a);

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
		free(a);
		return NULL;
	}

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		free(a);
		return bms_copy(b);
	}

	if (BMS_NWORDS(a) < BMS_NWORDS(b))
		a = (Bitmapset *) bms_realloc(a, BITMAPSET_SIZE(BMS_NWORDS(b)));

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
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}

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
			a = (Bitmapset *) bms_alloc0(offsetof(Bitmapset, data) + alloc_sz);
			a->nwords = (alloc_chunks_units << 16) | 0;
		}

		/*
		 * Iterate over chunk-aligned regions in the range. For each chunk
		 * that overlaps [lower, upper], expand, set the bits, encode, replace/insert.
		 */
		__sm_idx_t first_chunk = __sm_get_chunk_aligned_offset((size_t)lower);
		__sm_idx_t last_chunk = __sm_get_chunk_aligned_offset((size_t)upper);

		for (__sm_idx_t chunk_start = first_chunk; chunk_start <= last_chunk;
			 chunk_start += SM_CHUNK_MAX_CAPACITY)
		{
			size_t buf_off;
			int idx = bms_find_chunk(a, chunk_start, &buf_off);

			__sm_bitvec_t words[32];
			int cap_flags[32];
			size_t old_entry_size = 0;

			if (idx >= 0)
			{
				uint8_t *entry = BMS_BUF(a) + buf_off;
				old_entry_size = bms_chunk_entry_bytes(entry);
				__sm_chunk_t c;
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
			int64_t range_hi = (int64_t)chunk_start + SM_CHUNK_MAX_CAPACITY - 1;
			if (range_lo < lower)
				range_lo = lower;
			if (range_hi > upper)
				range_hi = upper;

			/* Set bits word by word within this chunk */
			for (int64_t bit = range_lo; bit <= range_hi; )
			{
				size_t within = (size_t)(bit - (int64_t)chunk_start);
				int slot = (int)(within / SM_BITS_PER_VECTOR);
				int bit_in_slot = (int)(within % SM_BITS_PER_VECTOR);

				cap_flags[slot] = 1;

				/* How many bits can we set in this slot? */
				int64_t slot_end = (int64_t)chunk_start + (int64_t)(slot + 1) * SM_BITS_PER_VECTOR - 1;
				if (slot_end > range_hi)
					slot_end = range_hi;
				int bit_end_in_slot = (int)((size_t)(slot_end - (int64_t)chunk_start) % SM_BITS_PER_VECTOR);

				if (bit_in_slot == 0 && bit_end_in_slot == SM_BITS_PER_VECTOR - 1)
				{
					/* Full word */
					words[slot] = ~(uint64_t)0;
				}
				else
				{
					/* Partial word - create mask */
					bitmapword lo_mask = ~(bitmapword)(((bitmapword)1 << bit_in_slot) - 1);
					int ush = SM_BITS_PER_VECTOR - (bit_end_in_slot + 1);
					bitmapword hi_mask = (~(bitmapword)0) >> ush;
					words[slot] |= lo_mask & hi_mask;
				}

				bit = slot_end + 1;
			}

			__sm_bitvec_t desc;
			__sm_bitvec_t vecs[32];
			int nvecs;
			__sm_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs);

			if (idx >= 0)
				a = bms_replace_chunk(a, buf_off, old_entry_size, chunk_start, desc, vecs, nvecs);
			else
				a = bms_insert_chunk(a, buf_off, chunk_start, desc, vecs, nvecs);
		}

		return a;
	}

	uwordnum = WORDNUM(upper);

	if (a == NULL)
	{
		a = (Bitmapset *) bms_alloc0(BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = (uint32_t)(uwordnum + 1);
	}
	else if (uwordnum >= BMS_NWORDS(a))
	{
		int			oldnwords = BMS_NWORDS(a);
		int64_t		i;

		a = (Bitmapset *) bms_realloc(a, BITMAPSET_SIZE(uwordnum + 1));
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
		free(a);
		return NULL;
	}

	if (BMS_IS_CHUNKED(a) || BMS_IS_CHUNKED(b))
	{
		Bitmapset *r = bms_intersect(a, b);
		free(a);
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
		free(a);
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
		free(a);
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
			free(a);
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
		free(a);
		if (b != a)
			free(b);
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
		free(other);

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
			__sm_idx_t chunk_start = bms_read_chunk_start(p);
			int64_t chunk_end = (int64_t)chunk_start + SM_CHUNK_MAX_CAPACITY - 1;

			if (chunk_end < target)
			{
				p += bms_chunk_entry_bytes(p);
				continue;
			}

			__sm_chunk_t c;
			bms_init_chunk_at(p, &c);

			__sm_bitvec_t words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			/* Search within this chunk starting from target */
			int64_t search_from = target;
			if (search_from < (int64_t)chunk_start)
				search_from = (int64_t)chunk_start;

			size_t within = (size_t)(search_from - (int64_t)chunk_start);
			int start_slot = (int)(within / SM_BITS_PER_VECTOR);
			int start_bit = (int)(within % SM_BITS_PER_VECTOR);

			for (int slot = start_slot; slot < 32; slot++)
			{
				bitmapword w = words[slot];
				if (slot == start_slot)
					w &= (~(bitmapword)0) << start_bit;

				if (w != 0)
				{
					int64_t result = (int64_t)chunk_start + (int64_t)slot * SM_BITS_PER_VECTOR;
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
			/* Start from the very last bit of the last chunk */
			const uint8_t *p = BMS_BUF(a);
			for (unsigned ci = 0; ci < used - 1; ci++)
				p += bms_chunk_entry_bytes(p);

			__sm_idx_t chunk_start = bms_read_chunk_start(p);
			target = (int64_t)chunk_start + SM_CHUNK_MAX_CAPACITY - 1;
		}
		else
		{
			target = prevbit - 1;
			if (target < 0)
				return -2;
		}

		/* Walk chunks in reverse, finding the right one */
		/* First, build an array of chunk offsets for reverse iteration */
		const uint8_t *base = BMS_BUF(a);
		const uint8_t *p = base;
		size_t offsets[used];
		for (unsigned ci = 0; ci < used; ci++)
		{
			offsets[ci] = (size_t)(p - base);
			p += bms_chunk_entry_bytes(p);
		}

		for (int ci = (int)used - 1; ci >= 0; ci--)
		{
			const uint8_t *entry = base + offsets[ci];
			__sm_idx_t chunk_start = bms_read_chunk_start(entry);

			if ((int64_t)chunk_start > target)
				continue;

			__sm_chunk_t c;
			bms_init_chunk_at(entry, &c);

			__sm_bitvec_t words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			size_t within_limit;
			if (target >= (int64_t)chunk_start + SM_CHUNK_MAX_CAPACITY)
				within_limit = SM_CHUNK_MAX_CAPACITY - 1;
			else
				within_limit = (size_t)(target - (int64_t)chunk_start);

			int end_slot = (int)(within_limit / SM_BITS_PER_VECTOR);
			int end_bit = (int)(within_limit % SM_BITS_PER_VECTOR);

			for (int slot = end_slot; slot >= 0; slot--)
			{
				bitmapword w = words[slot];
				if (slot == end_slot)
				{
					int shift = SM_BITS_PER_VECTOR - (end_bit + 1);
					w &= (~(bitmapword)0) >> shift;
				}

				if (w != 0)
				{
					int64_t result = (int64_t)chunk_start + (int64_t)slot * SM_BITS_PER_VECTOR;
					result += bmw_leftmost_one_pos(w);
					return result;
				}
			}
		}
		return -2;
	}

	assert(prevbit <= (int64_t)BMS_NWORDS(a) * BITS_PER_BITMAPWORD);
	assert(prevbit >= -1);

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
 * bms_hash_value - compute a hash key for a Bitmapset (FNV-1a)
 */
uint32_t
bms_hash_value(const Bitmapset *a)
{
	if (a == NULL)
		return 0;

	if (BMS_IS_CHUNKED(a))
	{
		/*
		 * For chunked mode, expand all chunks and hash the (chunk_start, word)
		 * pairs to produce a deterministic hash.
		 */
		uint32_t hash = 2166136261u;
		unsigned used = BMS_USED_CHUNKS(a);
		const uint8_t *p = BMS_BUF(a);

		for (unsigned ci = 0; ci < used; ci++)
		{
			__sm_idx_t chunk_start = bms_read_chunk_start(p);
			__sm_chunk_t c;
			bms_init_chunk_at(p, &c);
			__sm_bitvec_t words[32];
			int cap_flags[32];
			bms_expand_chunk_words(&c, words, cap_flags);

			/* Hash chunk start */
			unsigned char *sb = (unsigned char *)&chunk_start;
			for (size_t j = 0; j < sizeof(chunk_start); j++)
			{
				hash ^= sb[j];
				hash *= 16777619u;
			}

			/* Hash words */
			for (int w = 0; w < 32; w++)
			{
				unsigned char *wb = (unsigned char *)&words[w];
				for (size_t j = 0; j < sizeof(__sm_bitvec_t); j++)
				{
					hash ^= wb[j];
					hash *= 16777619u;
				}
			}

			p += bms_chunk_entry_bytes(p);
		}
		return hash;
	}

	uint32_t	hash = 2166136261u;

	for (int i = 0; i < BMS_NWORDS(a); i++)
	{
		unsigned char *bytes = (unsigned char *) &BMS_WORDS(a)[i];

		for (size_t j = 0; j < sizeof(bitmapword); j++)
		{
			hash ^= bytes[j];
			hash *= 16777619u;
		}
	}
	return hash;
}
