# DVS media engine — independent meson project (RK3588 RX side)

project('dvs_media', 'c',
  default_options: ['buildtype=release', 'warning_level=3']
)

cc = meson.get_compiler('c')

threads_dep = dependency('threads', required: true)
mtl_dep     = dependency('mtl', required: true)

common_c_args = ['-D_GNU_SOURCE', '-DALLOW_EXPERIMENTAL_API', '-Wno-pedantic']

common_inc = include_directories('../rk3588_common/include',
                                '../rk3588_common/include/rk3588_common')
file_playout_root  = '../rk3588_playout_rx'
file_playout_inc   = [include_directories(file_playout_root), common_inc,
             include_directories(file_playout_root + '/shared')]

# ---- Rockchip MPP probe ----
rockchip_mpp_dep = dependency('rockchip_mpp', required: false)
has_mpp = false
if rockchip_mpp_dep.found()
  mpp_code = '''
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
int main(void) { return 0; }
'''
  has_mpp = cc.links(mpp_code, dependencies: rockchip_mpp_dep,
                     name: 'rockchip mpp link test')
endif
if not has_mpp
  error('Rockchip MPP not found or does not link (RX side required)')
endif

librga_dep = dependency('librga', required: false)
has_tx_backend = has_mpp and librga_dep.found()

libdrm_dep    = dependency('libdrm', required: true)
libasound_dep = dependency('alsa', required: true)
libjson_c_dep = dependency('json-c', required: true)
libavformat_dep = dependency('libavformat', required: true)
libavcodec_dep  = dependency('libavcodec', required: true)
libavutil_dep   = dependency('libavutil', required: true)

# ---- shared infrastructure (reuse file_playout + rk3588_common) ----
dvs_shared = static_library('dvs_shared',
  files(
    '../rk3588_common/src/ring_buffer.c',
    '../rk3588_common/src/slab_pool.c',
    '../rk3588_common/src/timer.c',
    '../rk3588_common/src/sei_timestamp.c',
    '../rk3588_common/src/au_trace.c',
    '../rk3588_common/src/drm_mode.c',
    '../rk3588_common/src/display_layout.c',
    '../rk3588_common/src/ptp_time.c',
    file_playout_root + '/shared/rx_e2e_stats.c',
  ),
  c_args: common_c_args + ['-DHAVE_MTL=1'],
  dependencies: [threads_dep, mtl_dep],
  include_directories: [include_directories(file_playout_root + '/shared'), common_inc],
)

# ---- MP4 probe (FFmpeg) ----
file_playout_probe = static_library('file_playout_probe',
  files(file_playout_root + '/shared/mp4_demux.c'),
  c_args: common_c_args,
  dependencies: [libavformat_dep, libavcodec_dep, libavutil_dep],
  include_directories: file_playout_inc,
)

# ---- RX engine (reuse file_playout rx pipeline) ----
dvs_rx = static_library('dvs_rx',
  files(
    file_playout_root + '/rx/alsa_audio.c',
    file_playout_root + '/rx/playout_buffer.c',
    file_playout_root + '/rx/av_sync.c',
    file_playout_root + '/rx/media_clk.c',
    file_playout_root + '/rx/mtl_rx_mp4.c',
    file_playout_root + '/rx/mtl_rx_bridge.c',
    file_playout_root + '/rx/mtl_rx_audio.c',
    file_playout_root + '/shared/mpp_decoder.c',
    file_playout_root + '/shared/drm_renderer.c',
    file_playout_root + '/rx/h264_prep.c',
    '../rk3588_common/src/extradata_handler.c',
    file_playout_root + '/rx/rx_pipeline.c',
    file_playout_root + '/rx/rx_compositor.c',
  ),
  c_args: common_c_args,
  dependencies: [libasound_dep, mtl_dep, threads_dep,
                 rockchip_mpp_dep, libdrm_dep],
  link_with: [dvs_shared, file_playout_probe],
  include_directories: file_playout_inc,
)

live_ingest_root = '../rk3588_live_tx'
live_ingest_inc = [include_directories(live_ingest_root),
                   include_directories(live_ingest_root + '/codec'),
                   common_inc]

# ---- TX data plane (reuse live_ingest capture/encode/TX, in-tree, no copy)
# Application orchestration (live_ingest_app) and the decoder stay out;
# TX needs only the encode chain plus mtl_tx/tx_pipeline.
dvs_tx = []
engine_tx_backend_src = []
engine_tx_backend_args = []
if has_tx_backend
  dvs_tx = static_library('dvs_tx',
    files(
      '../rk3588_common/src/sei_timestamp.c',
      '../rk3588_common/src/sps_vui.c',
      '../rk3588_common/src/tx_tai_ts.c',
      '../rk3588_common/src/au_trace.c',
      live_ingest_root + '/hal/dma_allocator.c',
      live_ingest_root + '/hal/dma32_allocator.c',
      live_ingest_root + '/io/v4l2_capturer.c',
      live_ingest_root + '/codec/rga_converter.c',
      live_ingest_root + '/codec/mpp_encoder.c',
      live_ingest_root + '/pipeline/tx_pipeline.c',
      live_ingest_root + '/pipeline/tx_encode.c',
      live_ingest_root + '/pipeline/tx_supervise.c',
      live_ingest_root + '/pipeline/tx_stats.c',
      live_ingest_root + '/mtl_tx.c',
    ),
    c_args: common_c_args + ['-DHAVE_MTL=1'],
    dependencies: [threads_dep, mtl_dep, libdrm_dep, librga_dep,
                   rockchip_mpp_dep],
    link_with: dvs_shared,
    include_directories: [live_ingest_inc,
                          include_directories(live_ingest_root + '/pipeline'),
                          include_directories(live_ingest_root + '/hal'),
                          include_directories(live_ingest_root + '/io')],
  )
  engine_tx_backend_src = files('engine/tx_backend_live_ingest.c')
  engine_tx_backend_args = ['-DHAVE_TX_BACKEND=1']
endif

# ---- dvs-media app ----
dvs_media = executable('dvs_media',
  files(
    'dvs_media_main.c',
    'engine/engine_ctx.c',
    'engine/tx_session.c',
    'engine/tx_port_map.c',
    'engine/session_mgr.c',
    'engine/session_admit.c',
    'engine/layout_executor.c',
    'engine/audio_arbiter.c',
    'engine/audio_owner.c',
    'engine/stream_validate.c',
    'engine/base_image.c',
    'engine/ipc_engine_bridge.c',
    'engine/capability.c',
    'ipc/ipc_server.c',
    'ipc/state_machine.c',
    file_playout_root + '/file_playout_config.c',
  ) + engine_tx_backend_src,
  c_args: common_c_args + ['-DHAVE_LIBDRM=1'] + engine_tx_backend_args,
  dependencies: [mtl_dep, threads_dep, libasound_dep, libjson_c_dep,
                 libavformat_dep, libavcodec_dep, libavutil_dep,
                 rockchip_mpp_dep, libdrm_dep] + (has_tx_backend ? [librga_dep] : []),
  link_with: [dvs_shared, dvs_rx, file_playout_probe] + (has_tx_backend ? [dvs_tx] : []),
  include_directories: [include_directories('.'), file_playout_inc,
                        live_ingest_inc,
                        include_directories('../dvs_common/include')],
)

subdir('tests')

