/* SPDX-License-Identifier: MIT */
/*
 * sm_white.c - white-box test translation unit.
 *
 * The unit/property tests in test.c reach into sparsemap internals
 * (static helpers, the chunk codec, __sm_idx_t) that the public header
 * does not expose.  Rather than ship that scaffolding inside the
 * library, we compile it here: this file #includes the entire
 * implementation so it can see the file-static symbols, then defines
 * the QuickCheck (QCC) generators/printers and the _tst_* property
 * checks that test.c declares extern and drives.
 *
 * Built only by the test harness (tests/meson.build), never by the
 * installed library.  sm.c itself no longer contains any test code.
 */
#include "../sm.c"

#include <inttypes.h>

char *QCC_showSparsemap(void *value, int len);
char *QCC_showChunk(void *value, int len);
static char *_qcc_format_chunk(__sm_idx_t start, const __sm_chunk_t *chunk,
    bool none);

static void __attribute__((format(printf, 2, 3), unused))
__sm_diag_map(sparsemap_t *map, const char *fmt, ...)
{
	va_list args = { 0 };
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
	const char *s = QCC_showSparsemap(map, 0);
	fprintf(stdout, "\n%s\n", s);
	free((void *)s);
}

static void __attribute__((unused))
__sm_diag_chunk(const char *msg, __sm_chunk_t *chunk)
{
	const char *s = QCC_showChunk(chunk, 0);
	fprintf(stdout, "%s\n%s\n", msg, s);
	free((char *)s);
}

/* LCOV_EXCL_START
 *
 * Everything from here to the matching #endif is QCC-style property-
 * test scaffolding compiled in only when SPARSEMAP_TESTING is
 * defined.  These functions are reachable only from the test harness
 * (and only when QCC chooses to format a counterexample) so their
 * coverage is incidental to the library's correctness.  Excluded from
 * the coverage metric so the percentage reflects production code only.
 */

#include <qc.h>

static char *
_qcc_format_chunk(const __sm_idx_t start, const __sm_chunk_t *chunk,
    const bool none)
{
	size_t amt = sizeof(wchar_t) *
	    (SM_FLAGS_PER_INDEX * 16 + SM_BITS_PER_VECTOR * 64 + 16) * 2;
	char *buf = malloc(amt);

	const __sm_bitvec_t desc = chunk->m_data[0];

	if (!__sm_chunk_is_rle(chunk)) {
		char desc_str[(2 * SM_FLAGS_PER_INDEX + 1) *
		    sizeof(wchar_t)] = { 0 };
		char *str = desc_str;
		int mixed = 0;
		/* Loop bound: i in [0, SM_FLAGS_PER_INDEX).  The original
		 * `i <= SM_FLAGS_PER_INDEX` shifted by 2 * 32 = 64, which is UB
		 * on a 64-bit type and tripped UBSan when the diagnostic code
		 * fired on a property-test failure. */
		for (int i = 0; i < SM_FLAGS_PER_INDEX; i++) {
			const uint8_t flag = SM_CHUNK_GET_FLAGS(desc, i);
			switch (flag) {
			case SM_PAYLOAD_NONE:
				if (!none) {
					__sm_assert(flag == SM_PAYLOAD_NONE);
				}
				str += sprintf(str, "_");
				break;
			case SM_PAYLOAD_ONES:
				str += sprintf(str, "1");
				break;
			case SM_PAYLOAD_ZEROS:
				str += sprintf(str, "0");
				break;
			case SM_PAYLOAD_MIXED:
				str += sprintf(str, "X");
				mixed++;
				break;
			}
			if (i % 8 == 0 && i < 32) {
				str += sprintf(str, " ");
			}
		}
		str = buf +
		    sprintf(buf, "%.10u\t|%s|%s", start, desc_str,
		        mixed ? " :: " : "");
		for (int i = 0; i < mixed; i++) {
			const size_t n =
			    snprintf(str, amt - 1, "%#018" PRIx64 "%s",
			        chunk->m_data[1 + i], i + 1 < mixed ? " " : "");
			str += n;
			amt -= n;
		}
	} else {
		const size_t len = __sm_chunk_rle_get_length(chunk);
		const size_t cap = __sm_chunk_rle_get_capacity(chunk);
		sprintf(buf, "%.10u\t[%u, %zu) %zu of %zu", start, start,
		    start + len - 1, len, cap);
	}
	return buf;
}

char *
QCC_showChunk(void *value, int len)
{
	(void)len;
	const __sm_idx_t start = __sm_load_idx((const uint8_t *)value);
	__sm_chunk_t chunk;
	__sm_chunk_init(&chunk, value + SM_SIZEOF_OVERHEAD);

	return _qcc_format_chunk(start, &chunk, false);
}

char *
QCC_showSparsemap(void *value, int len)
{
	(void)len;
	char *buf = NULL;
	const sparsemap_t *map = (sparsemap_t *)value;
	const size_t count = __sm_get_chunk_count(map);

	if (count > 0) {
		char *str = NULL;
		uint8_t *p = __sm_get_chunk_data(map, 0);
		for (size_t i = 0; i < count; i++) {
			__sm_chunk_t chunk;
			const __sm_idx_t start =
			    __sm_load_idx((const uint8_t *)p);
			__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
			char *c = _qcc_format_chunk(start, &chunk, true);
			if (buf) {
				char *new_buf =
				    realloc(buf, strlen(buf) + strlen(c) + 24);
				if (new_buf) {
					buf = new_buf;
					str += sprintf(str, "\n%s", c);
				}
			} else {
				buf = c;
				str = buf + strlen(c);
			}
			p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		}
	}

	return buf;
}

static void
QCC_freeChunkValue(void *value)
{
	free(value);
}

static void
QCC_freeSparsemapValue(void *value)
{
	free(value);
}

QCC_GenValue *
QCC_genChunk()
{
	if ((double)random() / (double)RAND_MAX > 0.5) {
		// Generate a run-length encoded (RLE) chunk:
		const uint64_t from = 1, to = SM_CHUNK_RLE_MAX_LENGTH;
		const unsigned int len =
		    ((unsigned int)random() % (to - from)) + from;
		// First allocate enough room for the chunk data ...
		uint8_t *p =
		    malloc(SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
		// ... then set the offset to the length so we can test for that later ...
		__sm_store_idx((uint8_t *)p, len);
		// ... next is the chunk begins after the offset ...
		__sm_chunk_t chunk_local = {
			.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
			    SM_SIZEOF_OVERHEAD)
		};
		__sm_chunk_t *chunk = &chunk_local;
		// ... this contains a single vector ...
		chunk->m_data[0] = 0;
		// ... set the flags on this vector to indicate that is it RLE ...
		__sm_chunk_set_rle(chunk);
		// ... set the RLE chunk's initial capacity ...
		__sm_chunk_rle_set_capacity(chunk, SM_CHUNK_RLE_MAX_CAPACITY);
		// ... and set the RLE chunk's length of 1s to len.
		__sm_chunk_rle_set_length(chunk, len);
		// Now, test what we've generated to ensure it's correct.
		__sm_assert(__sm_load_idx(p) == len);
		__sm_assert(__sm_chunk_is_rle(chunk));
		__sm_assert(__sm_chunk_rle_get_capacity(chunk) ==
		    SM_CHUNK_RLE_MAX_CAPACITY);
		__sm_assert(__sm_chunk_rle_get_length(chunk) == len);
		return QCC_initGenValue(p, 1, QCC_showChunk,
		    QCC_freeChunkValue);
	}
	// Generate a chunk with the offset equal to the number of additional
	// vectors (len) and a descriptor that matches that with random data.
	const unsigned int from = 0, to = SM_FLAGS_PER_INDEX;
	const unsigned int len = ((unsigned int)random() % (to - from)) + from;
	const unsigned int cut =
	    ((unsigned int)random() % ((SM_FLAGS_PER_INDEX - len) - from)) +
	    from;
	// First allocate enough room for the chunk data ...
	uint8_t *p =
	    malloc(SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * (len + 1)));
	// ... then set the offset to the capacity ...
	__sm_store_idx((uint8_t *)p,
	    SM_CHUNK_MAX_CAPACITY - (cut * SM_BITS_PER_VECTOR));
	// ... next is the chunk begins after the offset ...
	__sm_chunk_t chunk_local = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};
	__sm_chunk_t *chunk = &chunk_local;
	// ... the first is the descriptor with the flags ...
	__sm_bitvec_unaligned_t *desc = chunk->m_data;
	*desc = 0;
	// ... ensure that exactly `len` flags are set to SM_PAYLOAD_MIXED ...
	for (size_t i = 0; i < len; i++) {
		SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_MIXED);
		/*
		 * The marker is `(uintptr_t)p + i` so that the test consumer can
		 * recompute it from the same buffer base, regardless of where
		 * the stack-local __sm_chunk_t happens to sit.
		 */
		chunk->m_data[1 + i] = (uintptr_t)p + i;
	}
	// ... and, on average, 50% of the rest are SM_PAYLOAD_ONES ...
	for (size_t i = len; i < SM_FLAGS_PER_INDEX - cut; i++) {
		const double coin = (double)random() / (double)RAND_MAX;
		if (SM_CHUNK_GET_FLAGS(*desc, i) != SM_PAYLOAD_MIXED &&
		    coin >= 0.5) {
			SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_ONES);
		}
	}
	// ... shuffle those around ...
	for (size_t i = 0; i < SM_FLAGS_PER_INDEX - cut - 1; i++) {
		const size_t range = SM_FLAGS_PER_INDEX - cut - i - 1;
		if (range == 0)
			break;
		const size_t j = i + 1 + ((size_t)random() % range);
		const int flags = SM_CHUNK_GET_FLAGS(*desc, j);
		SM_CHUNK_SET_FLAGS(*desc, j, SM_CHUNK_GET_FLAGS(*desc, i));
		SM_CHUNK_SET_FLAGS(*desc, i, flags);
	}
	// ... reduce the capacity by setting trailing flags to SM_PAYLOAD_NONE ...
	*desc <<= cut * 2;
	for (size_t i = 0; i < cut; i++) {
		SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_NONE);
	}
	// fprintf(stdout, "\n%s\n", QCC_showChunk(p, 0));
	// ... and check that our franken-chunk appears to be correct.
	__sm_assert(__sm_chunk_is_rle(chunk) == false);
	return QCC_initGenValue(p, 1, QCC_showChunk, QCC_freeChunkValue);
}

extern void populate_map(sparsemap_t *map, int size, int max_value);

QCC_GenValue *
QCC_genSparsemap()
{
	sparsemap_t *map = sparsemap(1024);
	return QCC_initGenValue(map, 1, QCC_showSparsemap,
	    QCC_freeSparsemapValue);
}

static size_t
_tst_sm_chunk_calc_vector_size(uint8_t b)
{
	int count = 0;

	for (int i = 0; i < 4; i++) {
		if (((b >> (i * 2)) & 0x03) == 0x02) {
			count++;
		}
	}

	return count;
}

QCC_TestStatus
_tst_chunk_calc_vector_size_equality(QCC_GenValue **vals, int len,
    QCC_Stamp **stamp)
{
	(void)len;
	(void)stamp;
	unsigned int a = *QCC_getValue(vals, 0, unsigned int *) % 256;
	if (_tst_sm_chunk_calc_vector_size(a) !=
	    __sm_chunk_calc_vector_size(a)) {
		return QCC_FAIL;
	}
	return QCC_OK;
}

QCC_TestStatus
_tst_chunk_get_position(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
	(void)len;
	(void)stamp;
	uint8_t *p = QCC_getValue(vals, 0, void *);
	/*
	 * The buffer's layout is: 4-byte start offset, then the chunk's
	 * bitvec data (descriptor + optional vectors).  Construct a
	 * stack-local __sm_chunk_t pointing at the bitvecs; do NOT cast
	 * the buffer to __sm_chunk_t * (which would interpret the
	 * descriptor as the m_data pointer).
	 */
	__sm_chunk_t chunk_local = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};
	__sm_chunk_t *chunk = &chunk_local;
	size_t pos;

	if (__sm_chunk_is_rle(chunk)) {
		for (size_t i = 0; i < SM_FLAGS_PER_INDEX; i++) {
			pos = __sm_chunk_get_position(chunk, i);
			if (pos != 0) {
				return QCC_FAIL;
			}
		}
	} else {
		size_t mixed = 0;
		for (size_t i = 0; i < SM_FLAGS_PER_INDEX; i++) {
			uint8_t flag = SM_CHUNK_GET_FLAGS(*chunk->m_data, i);
			switch (flag) {
			case SM_PAYLOAD_MIXED:
				pos = __sm_chunk_get_position(chunk, i);
				if (chunk->m_data[1 + pos] !=
				    (uintptr_t)p + pos) {
					return QCC_FAIL;
				}
				mixed++;
				break;
			case SM_PAYLOAD_ONES:
			case SM_PAYLOAD_ZEROS:
				pos = __sm_chunk_get_position(chunk, i);
				if (pos != mixed) {
					return QCC_FAIL;
				}
				break;
			case SM_PAYLOAD_NONE:
			default:
				break;
			}
		}
	}
	return QCC_OK;
}

QCC_TestStatus
_tst_chunk_get_capacity(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
	(void)len;
	(void)stamp;
	uint8_t *p = (uint8_t *)QCC_getValue(vals, 0, void *);
	__sm_idx_t start = __sm_load_idx((const uint8_t *)p);
	/* See _tst_chunk_get_position above for layout notes. */
	__sm_chunk_t chunk_local = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};
	__sm_chunk_t *chunk = &chunk_local;

	if (__sm_chunk_is_rle(chunk)) {
		if (__sm_chunk_rle_get_length(chunk) != start) {
			return QCC_FAIL;
		}
	} else {
		if (__sm_chunk_get_capacity(chunk) != start) {
			return QCC_FAIL;
		}
	}
	return QCC_OK;
}

QCC_TestStatus
_tst_get_chunk_offset(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
	const unsigned int idx = *QCC_getValue(vals, 0, unsigned int *);
	sparsemap_t *map = QCC_getValue(vals, 1, sparsemap_t *);
	const unsigned int max_offset =
	    (SM_FLAGS_PER_INDEX - 1) * sizeof(__sm_bitvec_t);
	const unsigned int rnd_offset =
	    (idx % max_offset) - (idx % max_offset % sizeof(__sm_bitvec_t));
	const unsigned int rnd_nvec = rnd_offset / sizeof(__sm_bitvec_t);
	const __sm_idx_t offset = __sm_get_chunk_aligned_offset(idx);
	ssize_t result;
	size_t expected;

	(void)len;
	(void)stamp;

#define FAIL_AT(line, ...)                                                     \
	do {                                                                   \
		fprintf(stderr, "FAIL at line %d (test input idx=%u): ", line, \
		    idx);                                                      \
		fprintf(stderr, __VA_ARGS__);                                  \
		fprintf(stderr, "\n");                                         \
		return QCC_FAIL;                                               \
	} while (0)

	// An empty map should return -1 (no chunks present, so offset of -1).
	for (unsigned int i = offset; i < SM_CHUNK_MAX_CAPACITY + offset; i++) {
		ssize_t result = __sm_get_chunk_offset(map, i);
		if (result != -1) {
			FAIL_AT(__LINE__,
			    "empty map check failed: __sm_get_chunk_offset(map, %u) = %zd, expected -1",
			    i, result);
		}
	}

	// By setting the first bit in each of rnd_nvec chunks we create one chunk
	// per and with exactly one additional bitvec per so we should observe...
	for (unsigned int i = 0; i < rnd_nvec; i++) {
		uint64_t l = offset + (i * SM_CHUNK_MAX_CAPACITY);
		sm_add(map, l);
	}
	for (unsigned int i = 0; i < rnd_nvec; i++) {
		size_t expected_offset = __sm_get_chunk_offset(map,
		    offset + (i * SM_CHUNK_MAX_CAPACITY));
		size_t calculated_offset =
		    i * (SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
		if (calculated_offset != expected_offset) {
			FAIL_AT(__LINE__,
			    "%d-chunk offset mismatch: expected=%zu, got=%zu",
			    i, calculated_offset, expected_offset);
		}
	}

	// Now for RLE, first let's clear and check a full chunk.
	sm_clear(map);
	for (int i = 0; i < SM_CHUNK_MAX_CAPACITY; i++) {
		sm_add(map, i);
	}
	for (int i = 0; i < SM_CHUNK_MAX_CAPACITY; i++) {
		ssize_t result = __sm_get_chunk_offset(map, i);
		if (result != 0) {
			FAIL_AT(__LINE__,
			    "RLE full chunk check failed: __sm_get_chunk_offset(map, %d) = %zd, expected 0",
			    i, result);
		}
	}
	result = __sm_get_chunk_offset(map, SM_CHUNK_MAX_CAPACITY);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset at boundary (before RLE transform) failed: __sm_get_chunk_offset(map, %d) = %zd, expected 0",
		    SM_CHUNK_MAX_CAPACITY, result);
	}

	// This should trigger the transformation of the 0th chunk into RLE.
	sm_add(map, SM_CHUNK_MAX_CAPACITY);
	result = __sm_get_chunk_offset(map, SM_CHUNK_MAX_CAPACITY);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after RLE transform failed: __sm_get_chunk_offset(map, %d) = %zd, expected 0",
		    SM_CHUNK_MAX_CAPACITY, result);
	}
	// This should trigger the transformation of the 0th chunk back to sparse.
	sm_remove(map, SM_CHUNK_MAX_CAPACITY);
	result = __sm_get_chunk_offset(map, SM_CHUNK_MAX_CAPACITY);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after sparse transform failed: __sm_get_chunk_offset(map, %d) = %zd, expected 0",
		    SM_CHUNK_MAX_CAPACITY, result);
	}

	// This should trigger the transformation of the 0th chunk into RLE again.
	for (int i = 0; i < 3000; i++) {
		sm_add(map, SM_CHUNK_MAX_CAPACITY + i);
	}

#ifdef SPARSEMAP_DIAGNOSTIC
	// Debug: check state after setting 3000 bits
	{
		size_t chunk_count = __sm_get_chunk_count(map);
		__sm_diag("After set 3000 bits (2048-5047): chunk_count=%zu\n",
		    chunk_count);
		uint8_t *p = __sm_get_chunk_data(map, 0);
		for (size_t i = 0; i < chunk_count; i++) {
			__sm_idx_t start = __sm_load_idx((const uint8_t *)p);
			__sm_chunk_t chunk;
			__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
			size_t cap = __sm_chunk_get_capacity(&chunk);
			bool is_rle = __sm_chunk_is_rle(&chunk);
			size_t len =
			    is_rle ? __sm_chunk_rle_get_length(&chunk) : 0;
			__sm_diag(
			    "  Chunk %zu: start=%u, capacity=%zu, length=%zu, RLE=%d\n",
			    i, start, cap, len, is_rle);
			p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		}
	}
#endif

	// This should trigger the transformation of the 0th chunk back to sparse,
	// but also create a second and third sparse chunks.
	sm_remove(map, 0);
	__sm_diag("After unset(0): chunk_count=%zu\n",
	    __sm_get_chunk_count(map));
	result = __sm_get_chunk_offset(map, 0);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after unset at 0 failed: __sm_get_chunk_offset(map, 0) = %zd, expected 0",
		    result);
	}
	sm_add(map, 0);
#ifdef SPARSEMAP_DIAGNOSTIC
	{
		size_t chunk_count = __sm_get_chunk_count(map);
		__sm_diag("After set(0): chunk_count=%zu\n", chunk_count);
		uint8_t *p = __sm_get_chunk_data(map, 0);
		__sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		bool is_rle = __sm_chunk_is_rle(&chunk);
		size_t cap = __sm_chunk_get_capacity(&chunk);
		size_t len = is_rle ? __sm_chunk_rle_get_length(&chunk) : 0;
		__sm_diag(
		    "  Chunk 0: start=%u, capacity=%zu, length=%zu, RLE=%d, m_data[0]=0x%llx\n",
		    start, cap, len, is_rle,
		    (unsigned long long)chunk.m_data[0]);
		// Verify some bits
		__sm_diag(
		    "  Bit checks: is_set(0)=%d, is_set(100)=%d, is_set(2050)=%d, is_set(5000)=%d\n",
		    sm_contains(map, 0), sm_contains(map, 100),
		    sm_contains(map, 2050), sm_contains(map, 5000));
	}
#endif

	// This will split the chunk into two chunks; sparse, RLE.
	__sm_diag("Before unset(129): chunk_count=%zu\n",
	    __sm_get_chunk_count(map));
	sm_remove(map, 129);
#ifdef SPARSEMAP_DIAGNOSTIC
	{
		size_t chunk_count = __sm_get_chunk_count(map);
		__sm_diag("After unset(129): chunk_count=%zu\n", chunk_count);
		for (size_t i = 0; i < chunk_count && i < 3; i++) {
			size_t chunk_offset = i == 0 ?
			    0 :
			    __sm_get_chunk_offset(map,
			        i * SM_CHUNK_MAX_CAPACITY);
			if ((ssize_t)chunk_offset == -1)
				break;
			uint8_t *p = __sm_get_chunk_data(map, chunk_offset);
			__sm_idx_t start = __sm_load_idx((const uint8_t *)p);
			__sm_chunk_t chunk;
			__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
			bool is_rle = __sm_chunk_is_rle(&chunk);
			size_t cap = __sm_chunk_get_capacity(&chunk);
			size_t len =
			    is_rle ? __sm_chunk_rle_get_length(&chunk) : 0;
			__sm_diag(
			    "  Chunk %zu: start=%u, capacity=%zu, length=%zu, RLE=%d\n",
			    i, start, cap, len, is_rle);
		}
	}
#endif
	result = __sm_get_chunk_offset(map, 129);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after split at 129 failed: __sm_get_chunk_offset(map, 129) = %zd, expected 0",
		    result);
	}
	sm_add(map, 129);
#ifdef SPARSEMAP_DIAGNOSTIC
	{
		size_t chunk_count = __sm_get_chunk_count(map);
		__sm_diag("After set(129): chunk_count=%zu\n", chunk_count);
		if (chunk_count > 0) {
			uint8_t *p = __sm_get_chunk_data(map, 0);
			__sm_idx_t start = __sm_load_idx((const uint8_t *)p);
			__sm_chunk_t chunk;
			__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
			bool is_rle = __sm_chunk_is_rle(&chunk);
			size_t cap = __sm_chunk_get_capacity(&chunk);
			size_t len =
			    is_rle ? __sm_chunk_rle_get_length(&chunk) : 0;
			__sm_diag(
			    "  Chunk 0: start=%u, capacity=%zu, length=%zu, RLE=%d\n",
			    start, cap, len, is_rle);
		}
	}
#endif

	// This will split the chunk into three chunks; sparse, sparse, RLE.
	__sm_diag("Before unset(2050): chunk_count=%zu\n",
	    __sm_get_chunk_count(map));
#ifdef SPARSEMAP_DIAGNOSTIC
	{
		uint8_t *p = __sm_get_chunk_data(map, 0);
		__sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		bool is_rle = __sm_chunk_is_rle(&chunk);
		size_t cap = __sm_chunk_get_capacity(&chunk);
		size_t len = is_rle ? __sm_chunk_rle_get_length(&chunk) : 0;
		__sm_diag(
		    "  Chunk 0: start=%u, capacity=%zu, length=%zu, RLE=%d\n",
		    start, cap, len, is_rle);
		__sm_diag(
		    "  Bit 2050 is_set=%d, idx 2050 in range [start=%u, start+length=%zu)? %d\n",
		    sm_contains(map, 2050), start, start + len,
		    (2050 >= start && 2050 < start + len));
	}
#endif
	sm_remove(map, 2050);

#ifdef SPARSEMAP_DIAGNOSTIC
	// Debug: check chunk count and structure
	{
		size_t chunk_count = __sm_get_chunk_count(map);
		__sm_diag("After unset(2050): chunk_count=%zu\n", chunk_count);
		uint8_t *p = __sm_get_chunk_data(map, 0);
		for (size_t i = 0; i < chunk_count; i++) {
			__sm_idx_t start = __sm_load_idx((const uint8_t *)p);
			__sm_chunk_t chunk;
			__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
			size_t capacity = __sm_chunk_get_capacity(&chunk);
			size_t size = __sm_chunk_get_size(&chunk);
			bool is_rle = __sm_chunk_is_rle(&chunk);
			__sm_diag(
			    "  Chunk %zu: start=%u, capacity=%zu, size=%zu, RLE=%d\n",
			    i, start, capacity, size, is_rle);
			p += SM_SIZEOF_OVERHEAD + size;
		}
	}
#endif

	result = __sm_get_chunk_offset(map, 0);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after 3-way split, chunk 0 failed: __sm_get_chunk_offset(map, 0) = %zd, expected 0",
		    result);
	}
	result = __sm_get_chunk_offset(map, 2050);
	expected = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
	if ((size_t)result != expected) {
		FAIL_AT(__LINE__,
		    "chunk offset after 3-way split, chunk 1 failed: __sm_get_chunk_offset(map, 2050) = %zd, expected %zu",
		    result, expected);
	}
	result = __sm_get_chunk_offset(map, 2050 + SM_CHUNK_MAX_CAPACITY);
	expected = 2 * SM_SIZEOF_OVERHEAD + 3 * sizeof(__sm_bitvec_t);
	if ((size_t)result != expected) {
		FAIL_AT(__LINE__,
		    "chunk offset after 3-way split, chunk 2 failed: __sm_get_chunk_offset(map, %d) = %zd, expected %zu",
		    2050 + SM_CHUNK_MAX_CAPACITY, result, expected);
	}
	sm_add(map, 2050);

	// This won't split the chunk, it just shrinks the RLE by one.
	sm_remove(map, 5047);
	result = __sm_get_chunk_offset(map, 5046);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after RLE shrink failed: __sm_get_chunk_offset(map, 5046) = %zd, expected 0",
		    result);
	}

	// This will split the chunk, the index is outside the range but inside the capacity.
	sm_add(map, 5048);
	result = __sm_get_chunk_offset(map, 4090);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after split, first chunk failed: __sm_get_chunk_offset(map, 4090) = %zd, expected 0",
		    result);
	}
	result = __sm_get_chunk_offset(map, 5046);
	if (result != 12) {
		FAIL_AT(__LINE__,
		    "chunk offset after split, second chunk failed: __sm_get_chunk_offset(map, 5046) = %zd, expected 12",
		    result);
	}

	sm_remove(map, 5048);
	result = __sm_get_chunk_offset(map, 5046);
	if (result != 0) {
		FAIL_AT(__LINE__,
		    "chunk offset after unset 5048 failed: __sm_get_chunk_offset(map, 5046) = %zd, expected 0",
		    result);
	}

#undef FAIL_AT
	return QCC_OK;
}

QCC_TestStatus
_tst_rle_select_rank_consistency(QCC_GenValue **vals, int len,
    QCC_Stamp **stamp)
{
	(void)len;
	(void)stamp;

	sparsemap_t *map = QCC_getValue(vals, 0, sparsemap_t *);
	if (!map || !map->m_data) {
		return QCC_OK;
	}

	/* Test property: for any set bit at index i, select(rank(0, i, true) - 1, true) should equal i
	 * This verifies that rank and select are inverse operations. */

	size_t count = sm_cardinality(map);
	if (count == 0) {
		return QCC_OK;
	}

	/* Sample up to 100 set bits to test */
	size_t test_limit = count < 100 ? count : 100;

	for (size_t n = 0; n < test_limit; n++) {
		/* Get the nth set bit */
		uint64_t idx = sm_select(map, n, true);
		if (!SM_FOUND(idx)) {
			break; /* No more set bits */
		}

		/* Verify the bit is actually set */
		if (!sm_contains(map, idx)) {
			return QCC_FAIL;
		}

		/* Get rank up to this index */
		int r = sm_rank(map, 0, idx, true);
		if (r <= 0) {
			return QCC_FAIL;
		}

		/* Select should give us back the same index */
		__sm_idx_t idx2 = sm_select(map, r - 1, true);
		if (idx2 != idx) {
			return QCC_FAIL;
		}
	}

	return QCC_OK;
}

/* Helper for scan completeness test */
static size_t scan_completeness_count = 0;
static void
_scan_completeness_counter(uint32_t v[], size_t n, void *aux)
{
	(void)v;
	(void)aux;
	scan_completeness_count += n;
}

QCC_TestStatus
_tst_rle_scan_completeness(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
	(void)len;
	(void)stamp;

	sparsemap_t *map = QCC_getValue(vals, 0, sparsemap_t *);
	if (!map || !map->m_data) {
		return QCC_OK;
	}

	/* Test property: scan must visit exactly count() set bits, no more, no less */

	size_t expected_count = sm_cardinality(map);

	/* Reset counter and scan */
	scan_completeness_count = 0;
	sm_scan(map, _scan_completeness_counter, 0, NULL);

	if (scan_completeness_count != expected_count) {
		return QCC_FAIL;
	}

	return QCC_OK;
}

/* LCOV_EXCL_STOP */
