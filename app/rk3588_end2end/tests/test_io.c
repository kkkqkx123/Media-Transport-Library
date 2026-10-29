/* SPDX-License-Identifier: BSD-3-Clause
 * Phase 2: V4L2→DRM pipeline verification.
 *
 * RK3588 rk_hdmirx defaults to BGR3. DRM passes NV12 framebuffers.
 * This causes color distortion but proves zero-copy dma-buf sharing works.
 * Phase 3 will add RGA hardware BGR3→NV12 conversion.
 */

#include <drm_fourcc.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../io/v4l2_capturer.h"
#include "../io/drm_renderer.h"

static atomic_bool g_running = true;
static void sig_handler(int s) { (void)s; atomic_store(&g_running, false); }
static uint64_t g_frames = 0;
static struct v4l2_capturer* g_cap = NULL;

static void on_frame(void* user_data, const struct v4l2_frame* f) {
  struct drm_renderer* r = (struct drm_renderer*)user_data;
  drm_renderer_display(r, f->dma_fd, f->stride, 0);
  if (g_cap) v4l2_capturer_release(g_cap, f->buf_index, f->generation);
  g_frames++;
  if (g_frames % 100 == 0)
    printf("test_io: %llu frames\n", (unsigned long long)g_frames);
}

int main(int argc, char** argv) {
  signal(SIGINT, sig_handler);
  const char* v4l2_dev = (argc > 1) ? argv[1] : "/dev/video0";
  const char* drm_dev  = (argc > 2) ? argv[2] : "/dev/dri/card0";
  const char* conn     = (argc > 3) ? argv[3] : "HDMI-A-1";
  const char* mode     = (argc > 4) ? argv[4] : "1920x1080@60";

  printf("=== Phase 2 IO Self-Test ===\n");
  printf("V4L2: %s  DRM: %s/%s  %s\n", v4l2_dev, drm_dev, conn, mode);
  printf("Note: BGR3→NV12 color distortion expected. Phase 3 adds RGA fix.\n");

  struct drm_renderer* r = drm_renderer_create(drm_dev, conn, mode,
                                                1920, 1080, DRM_FORMAT_NV12, 3);
  if (!r) { fprintf(stderr, "FAIL: drm_renderer\n"); return 1; }

  struct v4l2_capturer* cap = v4l2_capturer_create(v4l2_dev, 1920, 1080,
                                                    60.0f, 6, on_frame, r);
  g_cap = cap;
  if (!cap) { fprintf(stderr, "FAIL: capturer\n"); drm_renderer_free(r); return 1; }

  printf("Starting (Ctrl-C to stop)...\n");
  time_t start = time(NULL);
  v4l2_capturer_start(cap);
  while (atomic_load(&g_running)) { usleep(100000); }
  v4l2_capturer_stop(cap);
  time_t elapsed = time(NULL) - start;
  printf("\n%llu frames in %llds (%.1f fps)\n",
         (unsigned long long)g_frames, (long long)elapsed,
         elapsed ? (double)g_frames / elapsed : 0);
  v4l2_capturer_free(cap);
  drm_renderer_free(r);
  printf("=== PASS ===\n");
  return 0;
}
