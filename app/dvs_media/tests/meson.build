# dvs_media unit tests (no MTL dependency needed for ipc/ modules)
test_dep = [threads_dep, libjson_c_dep]

test_state_machine = executable('test_state_machine',
  files('test_state_machine.c', '../ipc/state_machine.c'),
  c_args: common_c_args,
  dependencies: [threads_dep],
  include_directories: include_directories('..'),
)

test_ipc = executable('test_ipc',
  files('test_ipc.c', '../ipc/ipc_server.c', '../ipc/state_machine.c',
        '../engine/capability.c'),
  c_args: common_c_args,
  dependencies: test_dep,
  include_directories: [include_directories('..'),
                        include_directories('../../dvs_common/include')],
)

test_session_admit = executable('test_session_admit',
  files('test_session_admit.c', '../engine/session_admit.c'),
  c_args: common_c_args,
  dependencies: [threads_dep],
  include_directories: include_directories('..'),
)

test('dvs_state_machine', test_state_machine)
test('dvs_ipc', test_ipc, timeout: 30)
test('dvs_session_admit', test_session_admit)
