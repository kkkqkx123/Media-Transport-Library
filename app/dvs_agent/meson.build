project('dvs_agent', 'cpp', 'c',
  default_options: ['buildtype=release', 'cpp_std=c++17']
)

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

json_c_dep = dependency('json-c', required: true)
threads_dep = dependency('threads', required: true)
dl_dep = cc.find_library('dl', required: false)

common_src = '../dvs_common'

inc = include_directories('.', common_src + '/include', common_src + '/third_party/sqlite')

common_sources = files(
  common_src + '/src/http_server.cpp',
  common_src + '/src/http_client.cpp',
  common_src + '/src/sha256.cpp',
  common_src + '/src/dvs_contract.cpp',
  common_src + '/src/dvs_http_json.cpp',
  common_src + '/third_party/sqlite/sqlite3.c',
)

dvs_agent = executable('dvs_agent',
  files(
    'main.cpp',
    'agent_audit.cpp',
    'agent_auth.cpp',
    'agent_media.cpp',
    'agent_config_routes.cpp',
    'agent_media_routes.cpp',
    'agent_sys_routes.cpp',
    'agent_downlink.cpp',
    'local_shim.cpp',
    'node_store.cpp',
    'media_ipc.cpp',
    'maintenance.cpp',
    'registration.cpp',
    'network_apply.cpp',
    'tx_config.cpp',
  ) + common_sources,
  include_directories: inc,
  dependencies: [json_c_dep, threads_dep, dl_dep],
  c_args: ['-DSQLITE_THREADSAFE=1'],
  cpp_args: ['-D_GNU_SOURCE'],
)

# tests

test_exe = executable('dvs_agent_test',
  files('tests/test_basic.cpp', 'node_store.cpp', 'local_shim.cpp',
        'media_ipc.cpp', 'registration.cpp', 'network_apply.cpp',
        'tx_config.cpp') + common_sources,
  include_directories: inc,
  dependencies: [json_c_dep, threads_dep, dl_dep],
  c_args: ['-DSQLITE_THREADSAFE=1'],
)

test('dvs_agent_basic', test_exe)
