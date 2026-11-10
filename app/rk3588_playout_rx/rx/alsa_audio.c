#include "alsa_audio.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct alsa_audio {
    snd_pcm_t*        pcm;
    pthread_mutex_t   lock;
    char              device[64];
    unsigned long     sample_rate;
    unsigned int      channels;
    snd_pcm_uframes_t total_written;
    unsigned long     write_errors;
    double            playback_sec;   /* master clock, updated under lock */
};

static int alsa_hw_setup(snd_pcm_t* pcm) {
    int ret = snd_pcm_set_params(pcm,
                                 SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 2,
                                 48000,
                                 1,
                                 500000);
    if (ret < 0) return ret;
    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    ret = snd_pcm_sw_params_current(pcm, sw);
    if (ret < 0) return ret;
    ret = snd_pcm_sw_params_set_start_threshold(pcm, sw, 4800);
    if (ret < 0) return ret;
    ret = snd_pcm_sw_params_set_avail_min(pcm, sw, 48);
    if (ret < 0) return ret;
    ret = snd_pcm_sw_params(pcm, sw);
    if (ret < 0) return ret;
    return snd_pcm_prepare(pcm);
}

static void alsa_update_playback_clock(alsa_audio* a) {
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(a->pcm, &delay) < 0) {
        a->playback_sec = (double)a->total_written / (double)a->sample_rate;
        return;
    }
    snd_pcm_uframes_t pos = (a->total_written > (snd_pcm_uframes_t)delay)
                            ? (a->total_written - (snd_pcm_uframes_t)delay)
                            : 0;
    a->playback_sec = (double)pos / (double)a->sample_rate;
}

int alsa_audio_open(const char* device, alsa_audio** out) {
    *out = NULL;
    alsa_audio* a = calloc(1, sizeof(*a));
    if (!a) return -ENOMEM;

    pthread_mutex_init(&a->lock, NULL);

    snprintf(a->device, sizeof(a->device), "%s", device ? device : "default");
    int ret = snd_pcm_open(&a->pcm, a->device, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        fprintf(stderr, "alsa: cannot open %s: %s\n", a->device, snd_strerror(ret));
        pthread_mutex_destroy(&a->lock);
        free(a);
        return -ENODEV;
    }

    ret = alsa_hw_setup(a->pcm);
    if (ret < 0) {
        fprintf(stderr, "alsa: hw setup failed: %s\n", snd_strerror(ret));
        snd_pcm_close(a->pcm);
        pthread_mutex_destroy(&a->lock);
        free(a);
        return -EINVAL;
    }

    a->sample_rate   = 48000;
    a->channels      = 2;
    a->total_written = 0;
    a->playback_sec  = 0.0;

    printf("alsa: ready device=%s rate=%lu ch=%u\n",
           device, a->sample_rate, a->channels);
    *out = a;
    return 0;
}

int alsa_audio_write(alsa_audio* a, const int16_t* data, unsigned long frames) {
    if (!a || !data || !frames) return -EINVAL;

    pthread_mutex_lock(&a->lock);
    if (!a->pcm) {
        if (snd_pcm_open(&a->pcm, a->device, SND_PCM_STREAM_PLAYBACK, 0) != 0 ||
            alsa_hw_setup(a->pcm) != 0) {
            if (a->pcm) { snd_pcm_close(a->pcm); a->pcm = NULL; }
            a->write_errors++;
            pthread_mutex_unlock(&a->lock);
            return -ENODEV;
        }
    }
    long ret = snd_pcm_writei(a->pcm, data, frames);
    if (ret < 0) {
        int err = (int)ret;
        if (err == -EPIPE || err == -ESTRPIPE) {
            if (snd_pcm_recover(a->pcm, err, 0) == 0)
                ret = snd_pcm_writei(a->pcm, data, frames);
        } else if (err == -EBADFD) {
            if (snd_pcm_prepare(a->pcm) == 0)
                ret = snd_pcm_writei(a->pcm, data, frames);
        }
        if (ret < 0) {
            snd_pcm_t* old = a->pcm;
            a->pcm = NULL;
            if (old) snd_pcm_close(old);
            snd_pcm_t* np = NULL;
            if (snd_pcm_open(&np, a->device, SND_PCM_STREAM_PLAYBACK, 0) == 0 &&
                alsa_hw_setup(np) == 0) {
                a->pcm = np;
                ret = snd_pcm_writei(a->pcm, data, frames);
            } else {
                if (np) snd_pcm_close(np);
            }
        }
        if (ret < 0) {
            a->write_errors++;
            if (a->write_errors < 5 || (a->write_errors % 1000) == 0)
                fprintf(stderr, "alsa: write error: %s\n", snd_strerror((int)ret));
            pthread_mutex_unlock(&a->lock);
            return (int)ret;
        }
    }

    if (ret > 0) {
        a->total_written += (snd_pcm_uframes_t)ret;
        alsa_update_playback_clock(a);
    }

    pthread_mutex_unlock(&a->lock);
    return 0;
}

double alsa_audio_clock_get(const alsa_audio* a) {
    if (!a) return 0.0;

    pthread_mutex_lock((pthread_mutex_t*)&a->lock);
    double sec = a->playback_sec;
    pthread_mutex_unlock((pthread_mutex_t*)&a->lock);
    return sec;
}

void alsa_audio_close(alsa_audio* a) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    snd_pcm_uframes_t written = a->total_written;
    unsigned long errors = a->write_errors;
    if (a->pcm) {
        snd_pcm_drain(a->pcm);
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
    }
    printf("alsa: close written=%lu frames errors=%lu\n",
           (unsigned long)written, errors);
    pthread_mutex_unlock(&a->lock);
    pthread_mutex_destroy(&a->lock);
    free(a);
}
