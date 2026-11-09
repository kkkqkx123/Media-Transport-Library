#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    const char* dev = (argc > 1) ? argv[1] : "default";
    unsigned int rate = 48000;
    int channels = 2;
    snd_pcm_t* pcm = NULL;
    int rc = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "FAIL: open %s: %s\n", dev, snd_strerror(rc));
        return 1;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, channels);
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0);
    rc = snd_pcm_hw_params(pcm, params);
    if (rc < 0) {
        fprintf(stderr, "FAIL: hw_params: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        return 1;
    }

    printf("PASS: opened %s @ %u Hz %d ch S16_LE\n", dev, rate, channels);

    int frames = rate; /* 1 second */
    short* buf = calloc(frames * channels, sizeof(short));
    for (int i = 0; i < frames; i++) {
        double t = (double)i / (double)rate;
        short v = (short)(0.3 * 32767.0 * sin(2.0 * M_PI * 1000.0 * t));
        buf[i * channels] = v;
        buf[i * channels + 1] = v;
    }

    snd_pcm_sframes_t written = snd_pcm_writei(pcm, buf, frames);
    if (written < 0) {
        written = snd_pcm_recover(pcm, (int)written, 0);
        if (written < 0) {
            fprintf(stderr, "FAIL: writei: %s\n", snd_strerror((int)written));
            free(buf);
            snd_pcm_close(pcm);
            return 1;
        }
    }
    snd_pcm_drain(pcm);
    printf("PASS: played 1s 1kHz tone (%ld frames)\n", (long)written);

    /* enumerate */
    void** hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) >= 0) {
        printf("devices:\n");
        for (void** h = hints; *h; h++) {
            char* name = snd_device_name_get_hint(*h, "NAME");
            char* desc = snd_device_name_get_hint(*h, "DESC");
            if (name && strcmp(name, "null") != 0) {
                printf("  %s - %s\n", name, desc ? desc : "");
            }
            free(name);
            free(desc);
        }
        snd_device_name_free_hint(hints);
    }

    free(buf);
    snd_pcm_close(pcm);
    return 0;
}
