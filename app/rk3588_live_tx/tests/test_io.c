#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../io/v4l2_capturer.h"
#ifdef HAVE_ALSA
#include "../io/alsa_capturer.h"
#endif

#ifdef HAVE_ALSA
static int alsa_test(const char* dev);
#endif

static atomic_bool g_running = true;
static void sig_handler(int s) {
  (void)s;
  atomic_store(&g_running, false);
}
static uint64_t g_frames = 0;
static struct v4l2_capturer* g_cap = NULL;

static void on_frame(void* user_data, const struct v4l2_frame* f) {
  (void)user_data;
  /* Encode-side self-test: count captured frames, no display involved. */
  if (g_cap) v4l2_capturer_release(g_cap, f->buf_index, f->generation);
  g_frames++;
  if (g_frames % 100 == 0) printf("test_io: %llu frames\n", (unsigned long long)g_frames);
}

int main(int argc, char** argv) {
  signal(SIGINT, sig_handler);
  const char* v4l2_dev = (argc > 1) ? argv[1] : "/dev/video0";

  printf("=== Capture Self-Test (encode-only, no display) ===\n");
  printf("V4L2: %s\n", v4l2_dev);

  struct v4l2_capturer* cap =
      v4l2_capturer_create(v4l2_dev, 1920, 1080, 60.0f, 6, on_frame, NULL);
  g_cap = cap;
  if (!cap) {
    fprintf(stderr, "FAIL: capturer\n");
    return 1;
  }

  int rc = 0;
#ifdef HAVE_ALSA
  /* optional second arg: ALSA device (e.g. hw:0,0); omitted → audio skipped */
  if (argc > 2) rc = alsa_test(argv[2]);
#else
  if (argc > 2) printf("SKIP: built without HAVE_ALSA\n");
#endif

  printf("Starting (Ctrl-C to stop)...\n");
  time_t start = time(NULL);
  v4l2_capturer_start(cap);
  while (atomic_load(&g_running)) {
    usleep(100000);
  }
  v4l2_capturer_stop(cap);
  time_t elapsed = time(NULL) - start;
  printf("\n%llu frames in %llds (%.1f fps)\n", (unsigned long long)g_frames,
         (long long)elapsed, elapsed ? (double)g_frames / elapsed : 0);
  v4l2_capturer_free(cap);
  printf("=== PASS ===\n");
  return rc;
}

#ifdef HAVE_ALSA
/* ---- ALSA capture self-test: test_io <v4l2_dev> [alsa_dev] ---- */
static atomic_ulong a_blocks = 0;
static atomic_ulong a_bad_len = 0;
static atomic_ulong a_pts_back = 0;
static uint64_t a_last_pts = 0;
static size_t a_expect_bytes = 0;
static struct alsa_capturer* g_acap = NULL;

static void a_release(void* user_data, void* data) {
  (void)user_data;
  (void)data;
}

static void a_on_pcm(void* user_data, const void* data, size_t len, uint64_t pts_ns,
                     void (*release)(void*, void*)) {
  (void)user_data;
  if (a_expect_bytes && len != a_expect_bytes) a_bad_len++;
  if (pts_ns && a_last_pts && pts_ns < a_last_pts) a_pts_back++;
  if (pts_ns) a_last_pts = pts_ns;
  a_blocks++;
  if (a_blocks % 100 == 0) printf("test_io: %lu audio blocks\n", (unsigned long)a_blocks);
  release(user_data, (void*)data);
}

static int alsa_test(const char* dev) {
  printf("\n=== ALSA Capture Self-Test (%s) ===\n", dev);
  struct alsa_capturer_params p = {
      .device = dev,
      .sample_rate = 48000,
      .channels = 2,
      .sample_bit = 16,
      .period_frames = 1024,
      .block_count = 8,
  };
  g_acap = alsa_capturer_create(&p, a_on_pcm, NULL);
  if (!g_acap) {
    printf("SKIP: ALSA device %s unavailable\n", dev);
    return 0;
  }
  a_expect_bytes = (size_t)alsa_capturer_period_bytes(g_acap);
  printf("negotiated: rate=%d ch=%d period_bytes=%zu\n",
         alsa_capturer_sample_rate(g_acap), alsa_capturer_channels(g_acap),
         a_expect_bytes);
  if (alsa_capturer_sample_rate(g_acap) != 48000) {
    printf("SKIP: device rate %d != 48000\n", alsa_capturer_sample_rate(g_acap));
    alsa_capturer_free(g_acap);
    return 0;
  }

  time_t start = time(NULL);
  if (alsa_capturer_start(g_acap) != 0) {
    fprintf(stderr, "FAIL: alsa start\n");
    alsa_capturer_free(g_acap);
    return 1;
  }
  sleep(3);
  alsa_capturer_stop(g_acap);
  time_t elapsed = time(NULL) - start;

  struct alsa_capturer_stats st;
  alsa_capturer_stats(g_acap, &st);
  printf(
      "%lu blocks in %llds (%lu bad_len, %lu pts_backward; "
      "drops=%lu xruns=%lu recovers=%lu errors=%lu)\n",
      (unsigned long)a_blocks, (long long)elapsed, (unsigned long)a_bad_len,
      (unsigned long)a_pts_back, (unsigned long)st.drops, (unsigned long)st.xruns,
      (unsigned long)st.recovers, (unsigned long)st.errors);
  alsa_capturer_free(g_acap);

  if (a_blocks == 0) {
    fprintf(stderr, "FAIL: no audio data\n");
    return 1;
  }
  if (a_bad_len) {
    fprintf(stderr, "FAIL: bad block length\n");
    return 1;
  }
  printf("=== PASS (alsa) ===\n");
  return 0;
}
#endif
