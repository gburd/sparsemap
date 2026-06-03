# Micro-benchmarks for sparsemap.
#
# Only the dependency-free micro-benchmark lives in the primary repo;
# it links against libsparsemap alone, so the numbers are stable and
# reviewable.  The comparative benchmark (sparsemap vs. CRoaring vs. a
# plain bitmapset), which pulls in the ~1 MB vendored roaring.c and the
# plotting tooling, lives on the separate `bench/comparative` branch.
#
# Run pinned for repeatability:
#   taskset -c 2 nice -n -5 ./builddir/bench/sm_microbench

executable('sm_microbench',
  'sm_microbench.c',
  dependencies : [
    libsparsemap_dep,
    cc.find_library('rt', required : false),
  ],
  build_by_default : true,
)
