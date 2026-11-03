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
mp4_root  = '../rk3588_mp4_dist'
mp4_inc   = [include_directories(mp4_root), common_inc,
             include_directories(mp4_root + '/shared')]

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

libdrm_dep    = dependency('libdrm', required: true)
libasound_dep = dependency('alsa', required: true)
libjson_c_dep = dependency('json-c', required: true)
libavformat_dep = dependency('libavformat', required: true)
libavcodec_dep  = dependency('libavcodec', required: true)
libavutil_dep   = dependency('libavutil', required: true)

# ---- shared infrastructure (reuse mp4_dist + rk3588_common) ----
dvs_shared = static_library('dvs_shared',
  files(
    '../rk3588_common/src/ring_buffer.c',
    '../rk3588_common/src/slab_pool.c',
    '../rk3588_common/src/timer.c',
    mp4_root + '/shared/au_trace.c',
    mp4_root + '/shared/drm_mode.c',
    mp4_root + '/shared/display_layout.c',
    mp4_root + '/shared/ptp_time.c',
    mp4_root + '/shared/tx_tai_ts.c',
    mp4_root + '/shared/e2e_stamp.c',
    mp4_root + '/shared/rx_e2e_stats.c',
  ),
  c_args: common_c_args + ['-DHAVE_MTL=1'],
  dependencies: [threads_dep, mtl_dep],
  include_directories: [include_directories(mp4_root + '/shared'), common_inc],
)

# ---- MP4 probe (FFmpeg) ----
mp4_probe = static_library('mp4_probe',
  files(mp4_root + '/tx/mp4_demux.c'),
  c_args: common_c_args,
  dependencies: [libavformat_dep, libavcodec_dep, libavutil_dep],
  include_directories: mp4_inc,
)

# ---- RX engine (reuse mp4_dist rx pipeline) ----
dvs_rx = static_library('dvs_rx',
  files(
    mp4_root + '/rx/alsa_audio.c',
    mp4_root + '/rx/playout_buffer.c',
    mp4_root + '/rx/av_sync.c',
    mp4_root + '/rx/media_clk.c',
    mp4_root + '/rx/mtl_rx_mp4.c',
    mp4_root + '/rx/mtl_rx_bridge.c',
    mp4_root + '/rx/mtl_rx_audio.c',
    mp4_root + '/shared/mpp_decoder.c',
    mp4_root + '/shared/drm_renderer.c',
    mp4_root + '/rx/h264_prep.c',
    mp4_root + '/tx/extradata_handler.c',
    mp4_root + '/rx/rx_pipeline.c',
    mp4_root + '/rx/rx_compositor.c',
  ),
  c_args: common_c_args,
  dependencies: [libasound_dep, mtl_dep, threads_dep,
                 rockchip_mpp_dep, libdrm_dep],
  link_with: [dvs_shared, mp4_probe],
  include_directories: mp4_inc,
)

# ---- dvs-media app ----
dvs_media = executable('dvs_media',
  files(
    'dvs_media_main.c',
    'engine/engine_ctx.c',
    'engine/session_mgr.c',
    'engine/session_admit.c',
    'engine/layout_executor.c',
    'engine/audio_arbiter.c',
    'engine/ipc_engine_bridge.c',
    'engine/capability.c',
    'ipc/ipc_server.c',
    'ipc/state_machine.c',
    mp4_root + '/mp4_dist_config.c',
  ),
  c_args: common_c_args + ['-DHAVE_LIBDRM=1'],
  dependencies: [mtl_dep, threads_dep, libasound_dep, libjson_c_dep,
                 libavformat_dep, libavcodec_dep, libavutil_dep,
                 rockchip_mpp_dep, libdrm_dep],
  link_with: [dvs_shared, dvs_rx, mp4_probe],
  include_directories: [include_directories('.'), mp4_inc],
)

subdir('tests')

