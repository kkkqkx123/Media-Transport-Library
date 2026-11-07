/* Audio owner classification — pure string parsing, no hardware dependency.
 *
 * Mirrors the owner forms accepted by engine/audio_arbiter.c ("lane<N>",
 * bare decimal lane index, "sip:port", exact "sip", empty meaning mute) so
 * the classification itself stays x86-unit-testable while lane resolution
 * against the running compositor remains the authority in audio_arbiter.c. */

#ifndef _DVS_AUDIO_OWNER_H_
#define _DVS_AUDIO_OWNER_H_

typedef enum {
    DVS_OWNER_MUTE = 0, /* empty string: mute, volume forced to 0 */
    DVS_OWNER_LANE,     /* "lane<N>" */
    DVS_OWNER_INDEX,    /* bare decimal lane index */
    DVS_OWNER_ENDPOINT, /* "sip:port" with a valid IPv4 address */
    DVS_OWNER_SIP,      /* exact IPv4 address */
    DVS_OWNER_INVALID   /* anything else: never falls back to another lane */
} dvs_audio_owner_kind_t;

/* Classify an owner string. NULL is treated as mute. */
dvs_audio_owner_kind_t dvs_audio_owner_classify(const char* owner);

/* Extract the lane index for the LANE/INDEX kinds. 0 on success, -1 for
 * any other kind or an out-of-range value. */
int dvs_audio_owner_lane_index(const char* owner, int* lane_out);

#endif /* _DVS_AUDIO_OWNER_H_ */
