TESTS = test test_rle_standalone test_heisenbug test_empty_map

check_PROGRAMS = test test_rle_standalone test_heisenbug test_empty_map soak

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/test
AM_LDFLAGS = -L$(top_builddir)/src
LDADD = $(top_builddir)/src/libsparsemap.la -lpthread -lm

# Main test suite — compiles its own copy of sparsemap.c with -DSPARSEMAP_TESTING
# so that QCC_genChunk, QCC_genSparsemap, and _tst_* symbols are available.
test_SOURCES = \
	test.c \
	common.c \
	common.h \
	munit.c \
	munit.h \
	qc.c \
	qc.h \
	roaring.c \
	roaring.h \
	tdigest.c \
	tdigest.h \
	midl.c \
	$(top_srcdir)/src/sparsemap.c

test_CFLAGS = $(AM_CFLAGS) -DSPARSEMAP_TESTING
test_LDADD = -lpthread -lm

# Standalone RLE test (links against library)
test_rle_standalone_SOURCES = test_rle_standalone.c
test_rle_standalone_CFLAGS = $(AM_CFLAGS)

# Heisenbug regression: sparsemap_set_data_size + wrap'd map.
# See HEISENBUG_REPORT.md and .agent/notes/sparsemap-cleanup-plan.md.
test_heisenbug_SOURCES = test_heisenbug.c
test_heisenbug_CFLAGS = $(AM_CFLAGS)

# Empty-map / m_data_used==0 regression.  See pg_tre's
# doc/sparsemap-bugfix-m_data_used-0.md and the consolidation plan.
test_empty_map_SOURCES = test_empty_map.c
test_empty_map_CFLAGS = $(AM_CFLAGS)

# Soak test (links against library)
soak_SOURCES = \
	soak.c \
	common.c \
	common.h \
	roaring.c \
	roaring.h \
	tdigest.c \
	tdigest.h

soak_CFLAGS = $(AM_CFLAGS)

# Benchmark (not in TESTS - it's a performance tool, not a pass/fail test)
check_PROGRAMS += bench

bench_SOURCES = \
	bench.c \
	roaring.c \
	roaring.h \
	tdigest.c \
	tdigest.h \
	bitmapset_standalone.c \
	bitmapset_standalone.h

bench_CFLAGS = $(AM_CFLAGS)

CLEANFILES = *.log *.trs
