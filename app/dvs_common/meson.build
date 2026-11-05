# DVS common infrastructure — shared C++17 utilities for dvs_agent and
# dvs_platform. Consumed in-tree by compiling these sources directly.

project('dvs_common', ['cpp', 'c'],
  default_options: ['buildtype=release', 'cpp_std=c++17']
)

cc = meson.get_compiler('c')

json_c_dep = dependency('json-c', required: false)
threads_dep = dependency('threads', required: true)
dl_dep = cc.find_library('dl', required: false)

inc = include_directories('include', 'third_party/sqlite')

dvs_common_sources = files(
  'src/http_server.cpp',
  'src/http_client.cpp',
  'src/sha256.cpp',
  'src/dvs_contract.cpp',
  'third_party/sqlite/sqlite3.c',
)

# Standalone syntax-check target (no consumers). Consumers compile the same
# sources in their own meson projects.
dvs_common = static_library('dvs_common',
  dvs_common_sources,
  include_directories: inc,
  dependencies: [threads_dep, dl_dep] + ([json_c_dep] if json_c_dep.found() else []),
  c_args: ['-DSQLITE_THREADSAFE=1'],
  cpp_args: ['-D_GNU_SOURCE'],
)
