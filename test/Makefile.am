TESTS = test test_rle_standalone

check_PROGRAMS = test test_rle_standalone soak

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

CLEANFILES = *.log *.trs
