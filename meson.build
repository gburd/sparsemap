project('sparsemap', 'c',
  version : '2.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
    # Define NDEBUG (stripping assert() in the hot path) for release
    # and plain builds, but keep asserts live for debug/debugoptimized.
    # Downstream consumers wanting max throughput should use
    # buildtype=release.
    'b_ndebug=if-release',
  ],
  license : 'MIT',
  meson_version : '>= 0.60.0',
)

cc = meson.get_compiler('c')

# Expose POSIX and GNU extensions (pthread support in the soak test,
# strdup, etc.).
add_project_arguments('-D_GNU_SOURCE', language : 'c')

# Project-wide include directory.
inc = include_directories('include')

# Optional diagnostic mode: enables __sm_diag() / __sm_assert() /
# __sm_check_invariants under SPARSEMAP_DIAGNOSTIC.  Off by default;
# turn on for development and CI.
if get_option('diagnostic')
  add_project_arguments('-DSPARSEMAP_DIAGNOSTIC=1', language : 'c')
endif

# Build the library, headers, and pkg-config from src/.
subdir('src')

# Build tests (controlled by -Dtesting=true|false; on by default).
if get_option('testing')
  subdir('tests')
endif

# Build benchmarks (controlled by -Dbenchmarks=true|false; on by default).
if get_option('benchmarks')
  subdir('bench')
endif

# Build examples (controlled by -Dexamples=true|false; on by default).
if get_option('examples')
  subdir('examples')
endif

# Optional Doxygen build (docs/api/html); silent if doxygen is missing.
doxygen = find_program('doxygen', required : false)
if doxygen.found()
  custom_target('doxygen',
    output : 'html',
    command : [doxygen, meson.project_source_root() / 'docs' / 'Doxyfile'],
    build_by_default : false,
  )
endif

# Install man pages.
install_man('man/sparsemap.3', 'man/sparsemap.7')
