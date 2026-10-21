# SPDX-License-Identifier: BSD-3-Clause
# MP4 Distribution over MTL ST22 — independent meson project
#
# RK3588 (TX + RX):
#   meson setup build && ninja -C build
#
# x86 / generic host (TX only — no Rockchip MPP):
#   meson setup build-tx -Dbuild_tx=true -Dbuild_rx=false && ninja -C build-tx
#   or: ./tests/build_verify.sh tx-only
#
# Requires meson_options.txt in this directory (defines build_tx / build_rx).

project('mp4_dist', 'c',
  default_options: ['buildtype=release', 'warning_level=3']
)

cc = meson.get_compiler('c')

build_tx = get_option('build_tx')
build_rx = get_option('build_rx')

if not build_tx and not build_rx
  error('At least one of build_tx or build_rx must be enabled')
endif

# ---- common dependencies ----
threads_dep = dependency('threads', required: true)
mtl_dep     = dependency('mtl', required: true)

common_c_args = ['-D_GNU_SOURCE', '-DALLOW_EXPERIMENTAL_API', '-Wno-pedantic']

# ---- FFmpeg + json-c (TX and/or RX config probe) ----
libavformat_dep = disabler()
libavcodec_dep  = disabler()
libavutil_dep   = disabler()
libjson_c_dep   = disabler()

if build_tx or build_rx
  libavformat_dep = dependency('libavformat', required: true)
  libavcodec_dep  = dependency('libavcodec', required: true)
  libavutil_dep   = dependency('libavutil', required: true)
  libjson_c_dep   = dependency('json-c', required: true)
endif

# ---- Rockchip MPP probe (never required for TX-only) ----
has_mpp = false
if build_rx
  rockchip_mpp_dep = dependency('rockchip_mpp', required: false)
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
    error('build_rx=true but Rockchip MPP not found or does not link.\n' +
          'On x86/generic hosts use: meson setup build-tx -Dbuild_tx=true -Dbuild_rx=false')
  endif
endif

# ============================================================
# Level 0: shared infrastructure (no MPP/DRM/ALSA)
# ============================================================
mp4_shared = static_library('mp4_shared',
  files(
    'shared/ring_buffer.c',
    'shared/slab_pool.c',
    'shared/timer.c',
    'shared/au_trace.c',
    'shared/drm_mode.c',
    'shared/display_layout.c',
    'shared/ptp_time.c',
    'shared/tx_tai_ts.c',
    'shared/e2e_stamp.c',
    'shared/rx_e2e_stats.c',
  ),
  c_args: common_c_args,
  dependencies: [threads_dep, mtl_dep],
)

test_tx_tai_ts = executable('test_tx_tai_ts',
  files('tests/test_tx_tai_ts.c'),
  c_args: common_c_args,
  dependencies: [mtl_dep],
  link_with: [mp4_shared],
)
test('tx_tai_ts', test_tx_tai_ts)

test_e2e_stamp = executable('test_e2e_stamp',
  files('tests/test_e2e_stamp.c'),
  c_args: common_c_args,
  link_with: [mp4_shared],
)
test('e2e_stamp', test_e2e_stamp)

# ============================================================
# MP4 probe (FFmpeg) — config + TX demux
# ============================================================
if build_tx or build_rx
  mp4_probe = static_library('mp4_probe',
    files('tx/mp4_demux.c'),
    c_args: common_c_args,
    dependencies: [libavformat_dep, libavcodec_dep, libavutil_dep],
  )
endif

# ============================================================
# TX: platform-independent (FFmpeg + MTL only)
# ============================================================
if build_tx
  mp4_tx = static_library('mp4_tx',
    files(
      'tx/extradata_handler.c',
      'tx/mtl_tx_video.c',
      'tx/mtl_tx_audio.c',
      'tx/mp4_playlist.c',
      'tx/tx_scheduler.c',
    ),
    c_args: common_c_args,
    dependencies: [libavformat_dep, libavcodec_dep, libavutil_dep, mtl_dep],
    link_with: [mp4_shared, mp4_probe],
  )

  mp4_dist_tx = executable('mp4_dist_tx',
    files('mp4_dist_config.c', 'mp4_tx_app.c'),
    c_args: common_c_args,
    dependencies: [mtl_dep, threads_dep, libjson_c_dep,
                   libavformat_dep, libavcodec_dep, libavutil_dep],
    link_with: [mp4_shared, mp4_probe, mp4_tx],
  )

  executable('test_mp4_dist',
    files('tests/test_mp4_dist.c'),
    c_args: common_c_args,
    dependencies: [libavformat_dep, libavcodec_dep, libavutil_dep, threads_dep],
    link_with: [mp4_shared, mp4_probe, mp4_tx],
  )
endif

# ============================================================
# RX: RK3588 only (MTL + MPP + DRM + ALSA)
# ============================================================
if build_rx
  libdrm_dep    = dependency('libdrm', required: true)
  libasound_dep = dependency('alsa', required: true)

  mp4_rx = static_library('mp4_rx',
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
      'tx/extradata_handler.c',
      'rx/rx_pipeline.c',
      'rx/rx_compositor.c',
    ),
    c_args: common_c_args,
    dependencies: [libasound_dep, mtl_dep, threads_dep,
                   rockchip_mpp_dep, libdrm_dep],
    link_with: [mp4_shared],
  )

  mp4_dist_rx = executable('mp4_dist_rx',
    files('mp4_dist_config.c', 'mp4_rx_app.c'),
    c_args: common_c_args,
    dependencies: [mtl_dep, threads_dep, libasound_dep, libjson_c_dep,
                   libavformat_dep, libavcodec_dep, libavutil_dep],
    link_with: [mp4_shared, mp4_probe, mp4_rx],
  )

  test_media_clk = executable('test_media_clk',
    files('tests/test_media_clk.c', 'rx/media_clk.c'),
    c_args: common_c_args,
    dependencies: [mtl_dep],
    include_directories: include_directories('.'),
  )
  test('media_clk', test_media_clk)

  executable('rx_minimal',
    files('tests/rx_minimal.c'),
    c_args: common_c_args,
    dependencies: [mtl_dep, threads_dep],
  )

  test_drm_mode = executable('test_drm_mode',
    files('tests/test_drm_mode.c'),
    c_args: common_c_args,
    link_with: [mp4_shared],
  )
  test('drm_mode', test_drm_mode)

  test_display_layout = executable('test_display_layout',
    files('tests/test_display_layout.c', 'shared/display_layout.c'),
    c_args: common_c_args,
  )
  test('display_layout', test_display_layout)

  executable('test_drm_plane',
    files('tests/test_drm_plane.c'),
    c_args: common_c_args,
    dependencies: [libdrm_dep],
  )
endif
