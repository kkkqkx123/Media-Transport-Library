# RK3588 live_ingest HDMI-MTL-HDMI — Phase 1-5 build

project('rk3588_live_ingest', 'c', default_options: ['buildtype=release', 'warning_level=3'])

cc = meson.get_compiler('c')

# ---- dependencies (discovered at build time) ----
libdrm_dep = dependency('libdrm', required: true)
libpthread_dep = dependency('threads')

# Optional: MPP (Phase 3+), MTL+json-c (Phase 5)
librockchip_mpp_dep = dependency('rockchip_mpp', required: false)
mtl_dep = dependency('mtl', required: false)
libjson_c_dep = dependency('json-c', required: false)

# ---- common shared utilities (single source of truth) ----
common_c_args = []
if mtl_dep.found()
  common_c_args += '-DHAVE_MTL=1'
endif
common_inc = include_directories('../rk3588_common/include',
                                '../rk3588_common/include/rk3588_common')
live_ingest_inc = [include_directories('.'), common_inc]

# ---- Phase 1: HAL (libdrm only) ----
# sps_vui is pure C (no hardware deps) so its test builds everywhere.
executable('test_sps_vui', 'tests/test_sps_vui.c',
  '../rk3588_common/src/sps_vui.c',
  include_directories: live_ingest_inc,
)

# RX tracking logic (seq gap / latency window / SEI offset): pure C test.
executable('test_rx_track', 'tests/test_rx_track.c',
  '../rk3588_common/src/sei_timestamp.c',
  include_directories: live_ingest_inc,
)

live_ingest_hal_sources = files(
  'hal/dma_allocator.c',
  'hal/dma32_allocator.c',
  '../rk3588_common/src/timer.c',
  '../rk3588_common/src/slab_pool.c',
  '../rk3588_common/src/sei_timestamp.c',
)

live_ingest_hal = static_library('live_ingest_hal', live_ingest_hal_sources,
  c_args: common_c_args,
  dependencies: [libdrm_dep],
  include_directories: live_ingest_inc,
)

executable('test_hal', 'tests/test_hal.c',
  link_with: live_ingest_hal,
  dependencies: [libdrm_dep],
  include_directories: live_ingest_inc,
)

# Slice-reassembler self-test (flush/timeout paths), x86-runnable.
executable('test_slice_reasm', 'pipeline/slice_reassembler.c',
  c_args: ['-DREASM_TEST'],
  link_with: live_ingest_hal,
  dependencies: [libdrm_dep, libpthread_dep],
  include_directories: live_ingest_inc,
)

# ---- Phase 2: IO (V4L2 capture + DRM display) ----
live_ingest_io_sources = files(
  'io/v4l2_capturer.c',
  'io/drm_renderer.c',
)

live_ingest_io = static_library('live_ingest_io', live_ingest_io_sources,
  link_with: live_ingest_hal,
  dependencies: [libdrm_dep, libpthread_dep],
  include_directories: live_ingest_inc,
)

executable('test_io', 'tests/test_io.c',
  link_with: [live_ingest_hal, live_ingest_io],
  dependencies: [libdrm_dep, libpthread_dep],
  include_directories: live_ingest_inc,
)

# ---- Phase 3: Codec (RGA + MPP) ----
# Note: librga and rockchip_mpp are required on RK3588.
# If not found, Phase 3 targets are skipped.
librga_dep = dependency('librga', required: false)
if librockchip_mpp_dep.found() and librga_dep.found()
  live_ingest_codec = static_library('live_ingest_codec',
    'codec/rga_converter.c',
    'codec/mpp_encoder.c',
    'codec/mpp_decoder.c',
    '../rk3588_common/src/sps_vui.c',
    link_with: live_ingest_hal,
    dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep],
    include_directories: live_ingest_inc,
  )

  executable('test_codec', 'tests/test_codec.c',
    link_with: [live_ingest_hal, live_ingest_io, live_ingest_codec],
    dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep],
    include_directories: live_ingest_inc,
  )
endif

# ---- Phase 4: Pipeline (ring buffers + TX/RX pipeline) ----
if librockchip_mpp_dep.found() and librga_dep.found()
  live_ingest_pipe_sources = files(
    '../rk3588_common/src/ring_buffer.c',
    'pipeline/slice_reassembler.c',
    'pipeline/tx_pipeline.c',
    'pipeline/rx_pipeline.c',
  )

  live_ingest_pipe_deps = [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep]
  if mtl_dep.found()
    live_ingest_pipe_deps += mtl_dep
  endif

  live_ingest_pipe = static_library('live_ingest_pipe', live_ingest_pipe_sources,
    link_with: [live_ingest_hal, live_ingest_codec, live_ingest_io],
    dependencies: live_ingest_pipe_deps,
    include_directories: live_ingest_inc,
  )

  test_pipeline_deps = [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep]
  if mtl_dep.found()
    test_pipeline_deps += mtl_dep
  endif

  executable('test_pipeline', ['tests/test_pipeline.c', 'mtl_rx.c', 'mtl_rx_bridge.c'],
    link_with: [live_ingest_hal, live_ingest_codec, live_ingest_io, live_ingest_pipe],
    dependencies: test_pipeline_deps,
    include_directories: live_ingest_inc,
  )
endif

# ---- Phase 5: live_ingest app with MTL (MTL + json-c required) ----
if librockchip_mpp_dep.found() and librga_dep.found() and mtl_dep.found() and libjson_c_dep.found()
  executable('live_ingest', files(
      'live_ingest_app.c',
      'live_ingest_config.c',
      'mtl_tx.c',
      'mtl_rx.c',
      'mtl_rx_bridge.c',
    ),
    link_with: [live_ingest_hal, live_ingest_codec, live_ingest_io, live_ingest_pipe],
    dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep,
                   mtl_dep, libjson_c_dep],
    include_directories: live_ingest_inc,
  )
endif
