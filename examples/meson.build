# Examples 3 and 4 use the test-side common.{c,h} helpers; they need
# to see <common.h> via the tests/ include path, and the compiled
# common.c needs to be in their link.
foreach n : ['ex_1', 'ex_2', 'ex_5', 'ex_6']
  executable(n,
    n + '.c',
    dependencies : libsparsemap_dep,
    c_args : ['-Wno-unused-parameter'],
    build_by_default : true,
  )
endforeach

foreach n : ['ex_3', 'ex_4']
  executable(n,
    n + '.c',
    '../tests/common.c',
    dependencies : libsparsemap_dep,
    include_directories : include_directories('../tests'),
    c_args : ['-Wno-unused-parameter'],
    build_by_default : true,
  )
endforeach
