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
        '../engine/capability.c', '../engine/tx_port_map.c',
        '../engine/session_admit.c'),
  c_args: common_c_args,
  dependencies: test_dep,
  include_directories: [include_directories('..'),
                        include_directories('../../dvs_common/include'),
                        file_playout_inc],
)

test_session_admit = executable('test_session_admit',
  files('test_session_admit.c', '../engine/session_admit.c'),
  c_args: common_c_args,
  dependencies: [threads_dep],
  include_directories: include_directories('..'),
)

test_tx_session = executable('test_tx_session',
  files('test_tx_session.c', '../engine/tx_session.c'),
  c_args: common_c_args,
  dependencies: [libjson_c_dep],
  include_directories: [include_directories('..'),
                        include_directories('../../dvs_common/include')],
)

test_tx_port_map = executable('test_tx_port_map',
  files('test_tx_port_map.c', '../engine/tx_port_map.c'),
  c_args: common_c_args,
  dependencies: [],
  include_directories: include_directories('..'),
)

test_stream_validate = executable('test_stream_validate',
  files('test_stream_validate.c', '../engine/stream_validate.c'),
  c_args: common_c_args,
  dependencies: [],
  include_directories: include_directories('..'),
)

test_audio_owner = executable('test_audio_owner',
  files('test_audio_owner.c', '../engine/audio_owner.c'),
  c_args: common_c_args,
  dependencies: [],
  include_directories: include_directories('..'),
)

test_capability_logic = executable('test_capability_logic',
  files('test_capability_logic.c', '../engine/capability.c',
        '../engine/tx_port_map.c', '../engine/session_admit.c'),
  c_args: common_c_args,
  dependencies: [threads_dep, libjson_c_dep],
  include_directories: [include_directories('..'),
                        include_directories('../../dvs_common/include'),
                        file_playout_inc],
)

test('dvs_state_machine', test_state_machine)
test('dvs_ipc', test_ipc, timeout: 30)
test('dvs_session_admit', test_session_admit)
test('dvs_tx_session', test_tx_session)
test('dvs_tx_port_map', test_tx_port_map)
test('dvs_stream_validate', test_stream_validate)
test('dvs_audio_owner', test_audio_owner)
test('dvs_capability_logic', test_capability_logic)
