project('dvs_agent', 'cpp', 'c',
  default_options: ['buildtype=release', 'cpp_std=c++17']
)

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

json_c_dep = dependency('json-c', required: true)
threads_dep = dependency('threads', required: true)
dl_dep = cc.find_library('dl', required: false)

sqlite_src = 'third_party/sqlite/sqlite3.c'

inc = include_directories('.', 'third_party/sqlite')

dvs_agent = executable('dvs_agent',
  files(
    'main.cpp',
    'node_store.cpp',
    'http_server.cpp',
    'media_ipc.cpp',
    'maintenance.cpp',
    'registration.cpp',
    'http_client.cpp',
    'sha256.cpp',
    sqlite_src,
  ),
  include_directories: inc,
  dependencies: [json_c_dep, threads_dep, dl_dep],
  c_args: ['-DSQLITE_THREADSAFE=1'],
  cpp_args: ['-D_GNU_SOURCE'],
)

# tests

test_exe = executable('dvs_agent_test',
  files('tests/test_basic.cpp', 'node_store.cpp', 'media_ipc.cpp',
        'registration.cpp', 'http_client.cpp', 'sha256.cpp', sqlite_src),
  include_directories: inc,
  dependencies: [json_c_dep, threads_dep, dl_dep],
  c_args: ['-DSQLITE_THREADSAFE=1'],
)

test('dvs_agent_basic', test_exe)
