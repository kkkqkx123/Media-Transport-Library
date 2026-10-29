# SPDX-License-Identifier: BSD-3-Clause
# RK3588 E2E HDMI-MTL-HDMI — Phase 1-5 build

project('rk3588_e2e', 'c', default_options: ['buildtype=release', 'warning_level=3'])

cc = meson.get_compiler('c')

# ---- dependencies (discovered at build time) ----
libdrm_dep = dependency('libdrm', required: true)
libpthread_dep = dependency('threads')

# Optional: MPP (Phase 3+), MTL+json-c (Phase 5)
librockchip_mpp_dep = dependency('rockchip_mpp', required: false)
mtl_dep = dependency('mtl', required: false)
libjson_c_dep = dependency('json-c', required: false)

# ---- Phase 1: HAL (libdrm only) ----
# sps_vui is pure C (no hardware deps) so its test builds everywhere.
executable('test_sps_vui', 'tests/test_sps_vui.c',
  'codec/sps_vui.c',
  include_directories: include_directories('.'),
)

e2e_hal_sources = files(
  'hal/dma_allocator.c',
  'hal/dma32_allocator.c',
  'hal/timer.c',
  'hal/slab_pool.c',
  'hal/sei_timestamp.c',
)

e2e_hal = static_library('e2e_hal', e2e_hal_sources,
  dependencies: [libdrm_dep],
  include_directories: include_directories('.'),
)

executable('test_hal', 'tests/test_hal.c',
  link_with: e2e_hal,
  dependencies: [libdrm_dep],
  include_directories: include_directories('.'),
)

# ---- Phase 2: IO (V4L2 capture + DRM display) ----
e2e_io_sources = files(
  'io/v4l2_capturer.c',
  'io/drm_renderer.c',
)

e2e_io = static_library('e2e_io', e2e_io_sources,
  link_with: e2e_hal,
  dependencies: [libdrm_dep, libpthread_dep],
  include_directories: include_directories('.'),
)

executable('test_io', 'tests/test_io.c',
  link_with: [e2e_hal, e2e_io],
  dependencies: [libdrm_dep, libpthread_dep],
  include_directories: include_directories('.'),
)

# ---- Phase 3: Codec (RGA + MPP) ----
# Note: librga and rockchip_mpp are required on RK3588.
# If not found, Phase 3 targets are skipped.
librga_dep = dependency('librga', required: false)
if librockchip_mpp_dep.found() and librga_dep.found()
  e2e_codec = static_library('e2e_codec',
    'codec/rga_converter.c',
    'codec/mpp_encoder.c',
    'codec/mpp_decoder.c',
    'codec/sps_vui.c',
    link_with: e2e_hal,
    dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep],
    include_directories: include_directories('.'),
  )

  executable('test_codec', 'tests/test_codec.c',
    link_with: [e2e_hal, e2e_io, e2e_codec],
    dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep],
    include_directories: include_directories('.'),
  )
endif

# ---- Phase 4: Pipeline (ring buffers + TX/RX pipeline) ----
if librockchip_mpp_dep.found() and librga_dep.found()
  e2e_pipe_sources = files(
    'pipeline/ring_buffer.c',
    'pipeline/slice_reassembler.c',
    'pipeline/tx_pipeline.c',
    'pipeline/rx_pipeline.c',
  )

  e2e_pipe_deps = [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep]
  if mtl_dep.found()
    e2e_pipe_deps += mtl_dep
  endif

  e2e_pipe = static_library('e2e_pipe', e2e_pipe_sources,
    link_with: [e2e_hal, e2e_codec, e2e_io],
    dependencies: e2e_pipe_deps,
    include_directories: include_directories('.'),
  )

  test_pipeline_deps = [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep]
  if mtl_dep.found()
    test_pipeline_deps += mtl_dep
  endif

  executable('test_pipeline', 'tests/test_pipeline.c',
    link_with: [e2e_hal, e2e_codec, e2e_io, e2e_pipe],
    dependencies: test_pipeline_deps,
    include_directories: include_directories('.'),
  )
endif

# ---- Phase 5: E2E app with MTL (MTL + json-c required) ----
if librockchip_mpp_dep.found() and librga_dep.found() and mtl_dep.found() and libjson_c_dep.found()
  executable('e2e_app', files(
      'e2e_app.c',
      'e2e_config.c',
      'mtl_tx.c',
      'mtl_rx.c',
      'mtl_rx_bridge.c',
    ),
    link_with: [e2e_hal, e2e_codec, e2e_io, e2e_pipe],
    dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep,
                   mtl_dep, libjson_c_dep],
    include_directories: include_directories('.'),
  )
endif
