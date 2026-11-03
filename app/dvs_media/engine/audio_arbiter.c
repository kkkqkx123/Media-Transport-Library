#include "audio_arbiter.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>

/* Apply a 0..100 linear volume to the ALSA "Master" playback element of the
 * given device. Returns 0 on success; negative ALSA error code otherwise.
 * Gracefully no-ops if the device has no Master element (soft-mute only). */
static int apply_volume(const char* device, int volume) {
    snd_mixer_t* mixer = NULL;
    snd_mixer_selem_id_t* sid = NULL;
    int rc = 0;

    if ((rc = snd_mixer_open(&mixer, 0)) < 0) goto out;
    if ((rc = snd_mixer_attach(mixer, device)) < 0) goto out;
    if ((rc = snd_mixer_selem_register(mixer, NULL, NULL)) < 0) goto out;
    if ((rc = snd_mixer_load(mixer)) < 0) goto out;

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    snd_mixer_elem_t* elem = snd_mixer_find_selem(mixer, sid);
    if (elem && snd_mixer_selem_has_playback_volume(elem)) {
        long min = 0, max = 0;
        snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        long v = min + (long)((double)(max - min) * volume / 100.0);
        if (v < min) v = min;
        if (v > max) v = max;
        snd_mixer_selem_set_playback_volume_all(elem, v);
    }
out:
    if (mixer) snd_mixer_close(mixer);
    return rc;
}

void dvs_audio_set(struct dvs_engine* e, const char* owner, int volume) {
    (void)e;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    const char* device = "default";
    if (owner && owner[0] != '\0') {
        if (strncmp(owner, "hw:", 3) == 0) device = owner;
    } else {
        volume = 0; /* no owner => mute */
    }

    int rc = apply_volume(device, volume);
    if (rc < 0)
        fprintf(stderr, "dvs_audio: ALSA volume apply failed (%s): %s\n", device,
                snd_strerror(rc));
}
