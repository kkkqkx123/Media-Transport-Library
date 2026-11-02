# RK3588 shared common utilities — additive-only shared static library.

project('rk3588_common', 'c', default_options: ['buildtype=release', 'warning_level=3'])

cc = meson.get_compiler('c')

threads_dep = dependency('threads', required: true)
mtl_dep = dependency('mtl', required: false)

common_c_args = []
common_deps = [threads_dep]
if mtl_dep.found()
  common_c_args += '-DHAVE_MTL=1'
  common_deps += mtl_dep
endif

rk3588_common = static_library('rk3588_common',
  files(
    'src/ring_buffer.c',
    'src/slab_pool.c',
    'src/timer.c',
  ),
  c_args: common_c_args,
  dependencies: common_deps,
  include_directories: [
    include_directories('include'),
    include_directories('include/rk3588_common'),
  ],
)

# Convenience alias for consumers building in-tree.
rk3588_common_dep = declare_dependency(
  link_with: rk3588_common,
  include_directories: include_directories('include'),
)
