lib_LTLIBRARIES = libsparsemap.la

libsparsemap_la_SOURCES = sparsemap.c
libsparsemap_la_CPPFLAGS = -I$(top_srcdir)/include
libsparsemap_la_LDFLAGS = -version-info 1:0:0

# Install public headers
include_HEADERS = \
	$(top_srcdir)/include/sparsemap.h \
	$(top_srcdir)/include/popcount.h

# Additional compiler flags
AM_CFLAGS = -Wall -Wextra
