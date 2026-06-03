BUILDDIR ?= builddir

.PHONY: setup build test clean reconfigure check coverage docs

setup:
	meson setup $(BUILDDIR)

build: setup
	ninja -C $(BUILDDIR)

# `make check` matches the autotools convention; it's a pure alias.
check: test

test: build
	meson test -C $(BUILDDIR) --print-errorlogs

# Coverage via scripts/measure_coverage.sh (gcov + lcov).
coverage:
	./scripts/measure_coverage.sh $(BUILDDIR)

docs:
	$(MAKE) -C docs doxygen

clean:
	rm -rf $(BUILDDIR)

reconfigure:
	meson setup --reconfigure $(BUILDDIR)
