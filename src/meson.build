libsparsemap = both_libraries('sparsemap',
  'sparsemap.c',
  include_directories : inc,
  install : true,
  version : meson.project_version(),
  soversion : '1',
)

libsparsemap_dep = declare_dependency(
  link_with : libsparsemap.get_static_lib(),
  include_directories : inc,
)

# Public headers.
install_headers(
  '../include/sparsemap.h',
  '../include/popcount.h',
  subdir : 'sparsemap',
)

# pkg-config .pc file generation.
pkg = import('pkgconfig')
pkg.generate(libsparsemap,
  name : 'sparsemap',
  description : 'Sparse, compressed bitmap with run-length encoding',
  version : meson.project_version(),
  url : 'https://codeberg.org/gregburd/sparsemap',
  subdirs : ['sparsemap'],
)
