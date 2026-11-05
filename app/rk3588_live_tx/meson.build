# RK3588 encode (TX-only) — capture/encode/send
#
# Cross-compile only (RK3588 hardware). All media dependencies are required;
# no desktop/x86 fallback is provided.
#
#   meson setup build && ninja -C build

project('rk3588_live_tx', 'c', default_options: ['buildtype=release', 'warning_level=3'])

cc = meson.get_compiler('c')

# ---- dependencies (all required on RK3588) ----
libdrm_dep = dependency('libdrm', required: true)
libpthread_dep = dependency('threads')
librockchip_mpp_dep = dependency('rockchip_mpp', required: true)
librga_dep = dependency('librga', required: true)
mtl_dep = dependency('mtl', required: true)
libjson_c_dep = dependency('json-c', required: true)
libavformat_dep = dependency('libavformat', required: true)
libavcodec_dep = dependency('libavcodec', required: true)
libavutil_dep = dependency('libavutil', required: true)

common_c_args = ['-DHAVE_MTL=1']
common_inc = include_directories('../rk3588_common/include',
                                '../rk3588_common/include/rk3588_common')
live_ingest_inc = [include_directories('.'), common_inc]

# ---- Phase 1: HAL (DMA + TX time/trace mechanisms from rk3588_common) ----
live_ingest_hal_sources = files(
  'hal/dma_allocator.c',
  'hal/dma32_allocator.c',
  '../rk3588_common/src/timer.c',
  '../rk3588_common/src/slab_pool.c',
  '../rk3588_common/src/sei_timestamp.c',
  '../rk3588_common/src/au_trace.c',
  '../rk3588_common/src/tx_tai_ts.c',
  '../rk3588_common/src/extradata_handler.c',
)

live_ingest_hal = static_library('live_ingest_hal', live_ingest_hal_sources,
  c_args: common_c_args,
  dependencies: [libdrm_dep, libpthread_dep, mtl_dep],
  include_directories: live_ingest_inc,
)

executable('test_hal', 'tests/test_hal.c',
  link_with: live_ingest_hal,
  dependencies: [libdrm_dep, libpthread_dep, mtl_dep],
  include_directories: live_ingest_inc,
)

# ---- Phase 2: IO (V4L2 capture only) ----
live_ingest_io = static_library('live_ingest_io',
  'io/v4l2_capturer.c',
  link_with: live_ingest_hal,
  dependencies: [libdrm_dep, libpthread_dep],
  include_directories: live_ingest_inc,
)

# ---- Phase 3: Codec (RGA + MPP encoder) ----
live_ingest_codec = static_library('live_ingest_codec',
  'codec/rga_converter.c',
  'codec/mpp_encoder.c',
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

# ---- Phase 4: Pipeline (TX only) ----
live_ingest_pipe = static_library('live_ingest_pipe',
  '../rk3588_common/src/ring_buffer.c',
  'pipeline/tx_pipeline.c',
  'mtl_tx_audio.c',
  link_with: [live_ingest_hal, live_ingest_codec, live_ingest_io],
  dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep, mtl_dep],
  include_directories: live_ingest_inc,
)

# ---- Phase 4b: File frame-source plugin (phase2, MP4 → send sink) ----
live_ingest_file = static_library('live_ingest_file',
  'file/file_source.c',
  link_with: [live_ingest_hal, live_ingest_pipe],
  dependencies: [libpthread_dep, mtl_dep,
                 libavformat_dep, libavcodec_dep, libavutil_dep],
  include_directories: live_ingest_inc,
)

executable('test_pipeline', 'tests/test_pipeline.c',
  link_with: [live_ingest_hal, live_ingest_codec, live_ingest_io, live_ingest_pipe],
  dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep, mtl_dep],
  include_directories: live_ingest_inc,
)

# ---- Phase 5: live_ingest TX app (hdmi + file sources) ----
executable('live_ingest', files(
    'live_ingest_app.c',
    'live_ingest_config.c',
    'mtl_tx.c',
  ),
  link_with: [live_ingest_hal, live_ingest_codec, live_ingest_io,
              live_ingest_pipe, live_ingest_file],
  dependencies: [libdrm_dep, librga_dep, librockchip_mpp_dep, libpthread_dep,
                 mtl_dep, libjson_c_dep,
                 libavformat_dep, libavcodec_dep, libavutil_dep],
  include_directories: live_ingest_inc,
)
