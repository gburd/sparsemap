TESTS = test test_rle_standalone

check_PROGRAMS = test test_rle_standalone soak

# Common flags for all tests
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/tests
AM_LDFLAGS = -L$(top_builddir)/src
LDADD = $(top_builddir)/src/libsparsemap.la -lpthread

# Main test suite
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
	midl.c

test_CFLAGS = $(AM_CFLAGS) -DSPARSEMAP_TESTING

# Standalone RLE test
test_rle_standalone_SOURCES = test_rle_standalone.c
test_rle_standalone_CFLAGS = $(AM_CFLAGS)

# Soak test
soak_SOURCES = \
	soak.c \
	common.c \
	common.h \
	roaring.c \
	roaring.h \
	tdigest.c \
	tdigest.h

soak_CFLAGS = $(AM_CFLAGS) -DSPARSEMAP_TESTING

# Clean up test artifacts
CLEANFILES = *.log *.trs
