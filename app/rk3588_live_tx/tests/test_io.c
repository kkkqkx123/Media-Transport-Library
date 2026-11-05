#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../io/v4l2_capturer.h"

static atomic_bool g_running = true;
static void sig_handler(int s) { (void)s; atomic_store(&g_running, false); }
static uint64_t g_frames = 0;
static struct v4l2_capturer* g_cap = NULL;

static void on_frame(void* user_data, const struct v4l2_frame* f) {
    (void)user_data;
    /* Encode-side self-test: count captured frames, no display involved. */
    if (g_cap) v4l2_capturer_release(g_cap, f->buf_index, f->generation);
    g_frames++;
    if (g_frames % 100 == 0)
        printf("test_io: %llu frames\n", (unsigned long long)g_frames);
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    const char* v4l2_dev = (argc > 1) ? argv[1] : "/dev/video0";

    printf("=== Capture Self-Test (encode-only, no display) ===\n");
    printf("V4L2: %s\n", v4l2_dev);

    struct v4l2_capturer* cap = v4l2_capturer_create(v4l2_dev, 1920, 1080,
                                                    60.0f, 6, on_frame, NULL);
    g_cap = cap;
    if (!cap) { fprintf(stderr, "FAIL: capturer\n"); return 1; }

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
    printf("=== PASS ===\n");
    return 0;
}
