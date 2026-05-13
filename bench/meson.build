# Comparative bench harness vs. CRoaring + a standalone bitmapset
# reference impl.  Reuses tests/{roaring,tdigest}.c as built oracles,
# linked via the support static library declared in tests/meson.build.

# Note: bench/ and tests/ live as siblings; we reach across by relative
# path for the support sources.  This avoids re-compiling the 1MB
# roaring.c twice.
bench_support = static_library('sparsemap_bench_support',
  '../tests/roaring.c',
  '../tests/tdigest.c',
  c_args : ['-Wno-unused-function', '-Wno-unused-parameter'],
)

executable('sparsemap_bench',
  'bench.c',
  'bitmapset_standalone.c',
  link_with : [bench_support],
  dependencies : [
    libsparsemap_dep,
    cc.find_library('m', required : false),
    dependency('threads'),
  ],
  include_directories : [inc, include_directories('../tests')],
  c_args : ['-Wno-unused-parameter', '-Wno-unused-function'],
  build_by_default : false,
)
