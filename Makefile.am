ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

if ENABLE_TESTING
SUBDIRS += test examples
endif

EXTRA_DIST = \
	README.md \
	LICENSE \
	doc/RLE_IMPLEMENTATION_SUMMARY.md \
	.clang-format \
	.clang-tidy \
	.editorconfig

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = sparsemap.pc

# Clean up generated files
CLEANFILES = *~

# Distribution cleanup
DISTCLEANFILES = \
	compile \
	config.guess \
	config.sub \
	depcomp \
	install-sh \
	ltmain.sh \
	missing

clean-local:
	-rm -rf autom4te.cache
	-rm -f aclocal.m4
	-rm -f config.h.in~
	-rm -f configure
	-find . -name Makefile.in -delete
	-find . -name .deps -type d -exec rm -rf {} +
	-find . -name .libs -type d -exec rm -rf {} +
