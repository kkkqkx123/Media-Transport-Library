#include "audio_arbiter.h"
#include "engine_ctx.h"

#include "rx/rx_compositor.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
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

static const char* play_device(const dvs_engine* e) {
    if (e && e->cfg.alsa_device[0]) return e->cfg.alsa_device;
    return "default";
}

int dvs_audio_resolve(const dvs_engine* e, const char* owner,
                      char* sip_out, size_t sip_len, uint16_t* port_out) {
    if (!e || !owner || !owner[0]) return -1;
    rx_compositor* comp = e->comp;
    if (!comp) return -1;
    int n = rx_compositor_lane_count(comp);
    if (n <= 0) return -1;

    /* "lane<N>" or bare decimal lane index. */
    const char* lp = owner;
    if (strncmp(owner, "lane", 4) == 0) lp = owner + 4;
    {
        char* end = NULL;
        long v = strtol(lp, &end, 10);
        if (end && end != lp && *end == '\0' && lp != owner) {
            /* "lane<N>" form. */
            for (int i = 0; i < n; i++) {
                const file_playout_pip_stream* st =
                    rx_compositor_lane_stream(comp, i);
                if (st && st->lane == (int)v) {
                    if (sip_out && sip_len)
                        snprintf(sip_out, sip_len, "%s", st->sip);
                    if (port_out) *port_out = st->audio_udp_port;
                    return (st->audio_udp_port != 0) ? 0 : -1;
                }
            }
            return -1;
        }
        if (lp == owner) {
            /* bare decimal: only accept when the whole string is numeric. */
            char* e2 = NULL;
            long v2 = strtol(owner, &e2, 10);
            if (e2 && e2 != owner && *e2 == '\0') {
                for (int i = 0; i < n; i++) {
                    const file_playout_pip_stream* st =
                        rx_compositor_lane_stream(comp, i);
                    if (st && st->lane == (int)v2) {
                        if (sip_out && sip_len)
                            snprintf(sip_out, sip_len, "%s", st->sip);
                        if (port_out) *port_out = st->audio_udp_port;
                        return (st->audio_udp_port != 0) ? 0 : -1;
                    }
                }
                return -1;
            }
        }
    }

    /* "sip:port" exact match. */
    const char* colon = strrchr(owner, ':');
    if (colon && colon != owner && colon[1]) {
        char ip[64] = {0};
        size_t ilen = (size_t)(colon - owner);
        if (ilen < sizeof(ip)) {
            memcpy(ip, owner, ilen);
            char* end = NULL;
            long pv = strtol(colon + 1, &end, 10);
            if (end && *end == '\0' && pv > 0 && pv < 65536) {
                for (int i = 0; i < n; i++) {
                    const file_playout_pip_stream* st =
                        rx_compositor_lane_stream(comp, i);
                    if (st && strcmp(st->sip, ip) == 0 &&
                        st->udp_port == (uint16_t)pv) {
                        if (sip_out && sip_len)
                            snprintf(sip_out, sip_len, "%s", st->sip);
                        if (port_out) *port_out = st->audio_udp_port;
                        return (st->audio_udp_port != 0) ? 0 : -1;
                    }
                }
                return -1;
            }
        }
    }

    /* exact sip match (first lane carrying it). */
    for (int i = 0; i < n; i++) {
        const file_playout_pip_stream* st = rx_compositor_lane_stream(comp, i);
        if (st && strcmp(st->sip, owner) == 0) {
            if (sip_out && sip_len)
                snprintf(sip_out, sip_len, "%s", st->sip);
            if (port_out) *port_out = st->audio_udp_port;
            return (st->audio_udp_port != 0) ? 0 : -1;
        }
    }
    return -1;
}

void dvs_audio_set(dvs_engine* e, const char* owner, int volume) {
    if (!e) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    const char* want = (owner && owner[0]) ? owner : "";
    int muted = (want[0] == '\0');

    if (muted) {
        if (e->comp)
            rx_compositor_audio_select(e->comp, NULL, 0);
        apply_volume(play_device(e), 0);
        snprintf(e->audio_applied_owner, sizeof(e->audio_applied_owner), "%s", "");
        e->audio_applied_sip[0] = '\0';
        e->audio_applied_port = 0;
        e->audio_applied_volume = 0;
        e->audio_applied_ok = 1;
        return;
    }

    char sip[32] = {0};
    uint16_t aport = 0;
    if (!e->comp || dvs_audio_resolve(e, want, sip, sizeof(sip), &aport) != 0) {
        fprintf(stderr, "dvs_audio: owner '%s' matches no lane audio port\n",
                want);
        /* A failed owner must not keep the previous lane audible while the
         * query reports not-applied: fall back to mute. */
        if (e->comp)
            rx_compositor_audio_select(e->comp, NULL, 0);
        apply_volume(play_device(e), 0);
        snprintf(e->audio_applied_owner, sizeof(e->audio_applied_owner), "%.63s",
                 want);
        e->audio_applied_sip[0] = '\0';
        e->audio_applied_port = 0;
        e->audio_applied_volume = volume;
        e->audio_applied_ok = 0;
        return;
    }
    int rc = 0;
    if (e->comp)
        rc = rx_compositor_audio_select(e->comp, sip, aport);
    if (rc != 0) {
        fprintf(stderr, "dvs_audio: subscription for '%s' (%s:%u) failed\n",
                want, sip, (unsigned)aport);
        snprintf(e->audio_applied_owner, sizeof(e->audio_applied_owner), "%.63s",
                 want);
        snprintf(e->audio_applied_sip, sizeof(e->audio_applied_sip), "%s", sip);
        e->audio_applied_port = aport;
        e->audio_applied_volume = volume;
        e->audio_applied_ok = 0;
        return;
    }
    int vrc = apply_volume(play_device(e), volume);
    if (vrc < 0)
        fprintf(stderr, "dvs_audio: ALSA volume apply failed (%s): %s\n",
                play_device(e), snd_strerror(vrc));
    snprintf(e->audio_applied_owner, sizeof(e->audio_applied_owner), "%.63s", want);
    snprintf(e->audio_applied_sip, sizeof(e->audio_applied_sip), "%s", sip);
    e->audio_applied_port = aport;
    e->audio_applied_volume = volume;
    e->audio_applied_ok = (vrc >= 0) ? 1 : 0;
}
