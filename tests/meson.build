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

# Main exhaustive test binary.  sm_white.c #includes the whole of
# ../sm.c so the property/unit tests can reach the file-static chunk
# codec; it also carries the QCC generators and _tst_* checks that
# test.c drives.  Compiled with -DSPARSEMAP_TESTING for the harness
# helpers in common.c.
test_main = executable('test_main',
  'test.c',
  'common.c',
  'qc.c',
  'midl.c',
  'sm_white.c',
  c_args : [
    '-DSPARSEMAP_TESTING',
    '-Wno-unused-parameter',
    '-Wno-unused-function',
  ],
  link_with : [support, munit],
  dependencies : [
    cc.find_library('m', required : false),
    dependency('threads'),
  ],
  include_directories : [inc, include_directories('.')],
)

# Standalone tests link against the library proper (not its source) so
# they exercise the public API exactly as a downstream consumer would.
foreach name : ['test_rle_standalone', 'test_heisenbug', 'test_empty_map', 'test_coverage', 'test_portability']
  exe = executable(name,
    name + '.c',
    dependencies : libsparsemap_dep,
    c_args : ['-Wno-unused-parameter'],
  )
  test(name, exe, timeout : 120)
endforeach

# The big munit-driven suite.  Long-running scale/perf cases live here.
test('main', test_main, timeout : 600)

# Symbol-prefix regression: compile a private copy of the library with
# -DSPARSEMAP_PREFIX and link a consumer that uses the renamed API.  Guards
# against the SPARSEMAP_PREFIX list in sm.h drifting out of sync with the
# exported symbol set.
test_prefix = executable('test_prefix',
  'test_prefix.c',
  '../sm.c',
  c_args : ['-DSPARSEMAP_PREFIX=smtest_'],
  include_directories : inc,
)
test('prefix', test_prefix, timeout : 60)

# Property tests using hegel-c (optional).  hegel-c has no pkg-config
# file and is usually vendored or installed by hand, so we probe for
# the header and the libraries it links against (-lhegel -lcbor -lz).
# With -Dhegel=auto the target is built only when all are present;
# -Dhegel=enabled turns a missing dependency into a hard error;
# -Dhegel=disabled skips it entirely.
hegel_opt = get_option('hegel')
if not hegel_opt.disabled()
  hegel_lib = cc.find_library('hegel', required : false)
  cbor_lib = cc.find_library('cbor', required : false)
  z_lib = cc.find_library('z', required : false)
  have_hegel = hegel_lib.found() and cbor_lib.found() and z_lib.found() \
    and cc.has_header('hegel/hegel.h')
  if have_hegel
    test_hegel = executable('test_hegel',
      'test_hegel.c',
      dependencies : [libsparsemap_dep, hegel_lib, cbor_lib, z_lib],
    )
    test('hegel', test_hegel, timeout : 300)
  elif hegel_opt.enabled()
    error('hegel=enabled but hegel-c (hegel/hegel.h + libhegel + libcbor + libz) was not found')
  endif
endif

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

# libFuzzer harness for the deserialization path.  Requires clang
# with -fsanitize=fuzzer support.  Disabled by default; enable with
#   meson setup builddir-fuzz -Dfuzz=enabled
fuzz_opt = get_option('fuzz')
if fuzz_opt.allowed()
  if cc.get_id() != 'clang'
    if fuzz_opt.enabled()
      error('fuzz requires clang (-fsanitize=fuzzer is clang-only)')
    endif
  else
    fuzz_args = ['-fsanitize=fuzzer,address,undefined', '-g', '-O1']
    executable('fuzz_deserialize',
      'fuzz_deserialize.c',
      dependencies : libsparsemap_dep,
      c_args : fuzz_args,
      link_args : fuzz_args,
      build_by_default : true,
    )
  endif
endif
