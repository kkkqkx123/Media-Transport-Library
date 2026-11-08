# dvs_platform — control-plane service (C++17, same stack as dvs_agent)

project('dvs_platform', ['cpp', 'c'],
  default_options: ['buildtype=release', 'cpp_std=c++17']
)

cc = meson.get_compiler('c')

json_c_dep = dependency('json-c', required: true)
threads_dep = dependency('threads', required: true)
dl_dep = cc.find_library('dl', required: false)

agent_src = '../dvs_agent'
common_src = '../dvs_common'
sqlite_src = common_src + '/third_party/sqlite/sqlite3.c'

inc = include_directories('.', agent_src, common_src + '/include',
                          common_src + '/third_party/sqlite')

common_sources = files(
  common_src + '/src/http_server.cpp',
  common_src + '/src/http_client.cpp',
  common_src + '/src/sha256.cpp',
  common_src + '/src/dvs_contract.cpp',
  sqlite_src,
)

platform_lib_src = files(
  'platform_store.cpp',
  'layout.cpp',
  'ops.cpp',
  'http_json.cpp',
  'docs_routes.cpp',
  'channels.cpp',
  'node_state.cpp',
  'layout_fanout.cpp',
  'barrier.cpp',
  'ws_channel.cpp',
  'node_routes.cpp',
  'op_routes.cpp',
  'media_routes.cpp',
  'upgrade_jobs.cpp',
  'ops_sweeper.cpp',
  'platform_server.cpp',
) + common_sources

dvs_platform = executable('dvs_platform',
  files('main.cpp') + platform_lib_src,
  include_directories: inc,
  dependencies: [json_c_dep, threads_dep, dl_dep],
  c_args: ['-DSQLITE_THREADSAFE=1'],
  cpp_args: ['-D_GNU_SOURCE'],
)

test_exe = executable('dvs_platform_test',
  files('tests/test_basic.cpp') + platform_lib_src,
  include_directories: inc,
  dependencies: [json_c_dep, threads_dep, dl_dep],
  c_args: ['-DSQLITE_THREADSAFE=1'],
)

test('dvs_platform_basic', test_exe)
