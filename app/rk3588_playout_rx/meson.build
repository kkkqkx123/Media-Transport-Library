# MP4 Reception over MTL ST22 — independent meson project
#
# RK3588 RX-only (decode/receive side). Cross-compile only (RK3588 hardware);
# no desktop/x86 fallback is provided.
#
#   meson setup build && ninja -C build

project('file_playout', 'c',
  default_options: ['buildtype=release', 'warning_level=3']
)

cc = meson.get_compiler('c')

# ---- common dependencies (all required on RK3588) ----
threads_dep = dependency('threads', required: true)
mtl_dep     = dependency('mtl', required: true)

common_c_args = ['-D_GNU_SOURCE', '-DALLOW_EXPERIMENTAL_API', '-Wno-pedantic']

# ---- common shared utilities (single source of truth) ----
common_inc = include_directories('../rk3588_common/include',
                                '../rk3588_common/include/rk3588_common')
file_playout_inc = [include_directories('.'), common_inc]

# ---- FFmpeg + json-c (RX probe helpers) ----
libavformat_dep = dependency('libavformat', required: true)
libavcodec_dep  = dependency('libavcodec', required: true)
libavutil_dep   = dependency('libavutil', required: true)
libjson_c_dep   = dependency('json-c', required: true)

# ---- Rockchip MPP (required, RX side) ----
rockchip_mpp_dep = dependency('rockchip_mpp', required: true)
mpp_code = '''
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
int main(void) { return 0; }
'''
if not cc.links(mpp_code, dependencies: rockchip_mpp_dep,
                name: 'rockchip mpp link test')
  error('Rockchip MPP not found or does not link (RX-only RK3588 app).')
endif

libdrm_dep    = dependency('libdrm', required: true)
libasound_dep = dependency('alsa', required: true)

# ============================================================
# Level 0: shared infrastructure (no MPP/DRM/ALSA)
# ============================================================
file_playout_shared = static_library('file_playout_shared',
  files(
    '../rk3588_common/src/ring_buffer.c',
    '../rk3588_common/src/slab_pool.c',
    '../rk3588_common/src/timer.c',
    '../rk3588_common/src/au_trace.c',
    '../rk3588_common/src/drm_mode.c',
    '../rk3588_common/src/display_layout.c',
    '../rk3588_common/src/ptp_time.c',
    '../rk3588_common/src/sei_timestamp.c',
    'shared/rx_e2e_stats.c',
  ),
  c_args: common_c_args + ['-DHAVE_MTL=1'],
  dependencies: [threads_dep, mtl_dep],
  include_directories: [include_directories('shared'), common_inc],
)

# ============================================================
# MP4 probe (FFmpeg) — decode-side reference stream descriptor
# ============================================================
file_playout_probe = static_library('file_playout_probe',
  files('shared/mp4_demux.c'),
  c_args: common_c_args,
  dependencies: [libavformat_dep, libavcodec_dep, libavutil_dep],
  include_directories: file_playout_inc,
)

# ============================================================
# RX: RK3588 (MTL + MPP + DRM + ALSA)
# ============================================================
file_playout_rx_lib = static_library('file_playout_rx_lib',
  files(
    'rx/alsa_audio.c',
    'rx/playout_buffer.c',
    'rx/av_sync.c',
    'rx/media_clk.c',
    'rx/mtl_rx_mp4.c',
    'rx/mtl_rx_bridge.c',
    'rx/mtl_rx_audio.c',
    'shared/mpp_decoder.c',
    'shared/drm_renderer.c',
    'rx/h264_prep.c',
    '../rk3588_common/src/extradata_handler.c',
    'rx/rx_pipeline.c',
    'rx/rx_compositor.c',
  ),
  c_args: common_c_args,
  dependencies: [libasound_dep, mtl_dep, threads_dep,
                 rockchip_mpp_dep, libdrm_dep],
  link_with: [file_playout_shared],
  include_directories: file_playout_inc,
)

file_playout_rx = executable('file_playout_rx',
  files('file_playout_config.c', 'file_playout_app.c'),
  c_args: common_c_args,
  dependencies: [mtl_dep, threads_dep, libasound_dep, libjson_c_dep,
                 libavformat_dep, libavcodec_dep, libavutil_dep],
  link_with: [file_playout_shared, file_playout_probe, file_playout_rx_lib],
  include_directories: file_playout_inc,
)

test_media_clk = executable('test_media_clk',
  files('tests/test_media_clk.c', 'rx/media_clk.c'),
  c_args: common_c_args,
  dependencies: [mtl_dep],
  include_directories: file_playout_inc,
)
test('media_clk', test_media_clk)

test_pacing_config = executable('test_pacing_config',
  files('tests/test_pacing_config.c', 'file_playout_config.c'),
  c_args: common_c_args,
  dependencies: [mtl_dep, libjson_c_dep],
  link_with: [file_playout_shared, file_playout_probe],
  include_directories: file_playout_inc,
)
test('pacing_config', test_pacing_config)

test_codec_config = executable('test_codec_config',
  files('tests/test_codec_config.c', 'file_playout_config.c'),
  c_args: common_c_args,
  dependencies: [mtl_dep, libjson_c_dep],
  link_with: [file_playout_shared, file_playout_probe],
  include_directories: file_playout_inc,
)
test('codec_config', test_codec_config)

executable('rx_minimal',
  files('tests/rx_minimal.c'),
  c_args: common_c_args,
  dependencies: [mtl_dep, threads_dep],
  include_directories: file_playout_inc,
)

test_drm_mode = executable('test_drm_mode',
  files('tests/test_drm_mode.c'),
  c_args: common_c_args,
  link_with: [file_playout_shared],
  include_directories: file_playout_inc,
)
test('drm_mode', test_drm_mode)

test_display_layout = executable('test_display_layout',
  files('tests/test_display_layout.c', '../rk3588_common/src/display_layout.c'),
  c_args: common_c_args,
  include_directories: file_playout_inc,
)
test('display_layout', test_display_layout)

executable('test_drm_plane',
  files('tests/test_drm_plane.c'),
  c_args: common_c_args,
  dependencies: [libdrm_dep],
  include_directories: file_playout_inc,
)
