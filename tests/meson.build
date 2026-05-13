# Reference implementations used as oracles by the property tests
# (roaring) and as performance reference samplers (tdigest).
# Built once as a shared static library so both the main test binary
# and the bench/ executables can link against it.
support = static_library('sparsemap_test_support',
  'roaring.c',
  'tdigest.c',
  c_args : ['-Wno-unused-function', '-Wno-unused-parameter'],
)

# munit: the test harness used by the main test/test.c suite.
munit = static_library('sparsemap_munit',
  'munit.c',
  c_args : ['-Wno-unused-parameter'],
)

# qc + helpers used inside the main test binary; built directly into
# the test executable so it can see SPARSEMAP_TESTING-only symbols.

# Main exhaustive test binary.  Compiles its own copy of sparsemap.c
# with -DSPARSEMAP_TESTING so QCC_genChunk, _tst_*, etc. are visible.
test_main = executable('test_main',
  'test.c',
  'common.c',
  'qc.c',
  'midl.c',
  '../src/sparsemap.c',
  c_args : [
    '-DSPARSEMAP_TESTING',
    '-U_FORTIFY_SOURCE',
    '-fno-stack-protector',
    '-Wno-unused-parameter',
    '-Wno-unused-function',
  ],
  link_args : ['-fno-stack-protector'],
  link_with : [support, munit],
  dependencies : [
    cc.find_library('m', required : false),
    dependency('threads'),
  ],
  include_directories : [inc, include_directories('.')],
)

# Standalone tests link against the library proper (not its source) so
# they exercise the public API exactly as a downstream consumer would.
foreach name : ['test_rle_standalone', 'test_heisenbug', 'test_empty_map']
  exe = executable(name,
    name + '.c',
    dependencies : libsparsemap_dep,
    c_args : ['-Wno-unused-parameter'],
  )
  test(name, exe, timeout : 120)
endforeach

# The big munit-driven suite.  Long-running scale/perf cases live here.
test('main', test_main, timeout : 600)

# Soak is a stochastic stress harness, not a pass/fail gate.  Build it
# (so it stays compiling) but don't add it to `meson test`; run on
# demand with `ninja -C build sparsemap_soak && ./build/tests/soak`.
executable('sparsemap_soak',
  'soak.c',
  'common.c',
  link_with : [support],
  dependencies : [
    libsparsemap_dep,
    cc.find_library('m', required : false),
    dependency('threads'),
  ],
  c_args : ['-Wno-unused-parameter', '-Wno-unused-function'],
  build_by_default : false,
)
