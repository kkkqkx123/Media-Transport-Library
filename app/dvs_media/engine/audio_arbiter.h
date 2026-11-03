#ifndef _DVS_AUDIO_ARBITER_H_
#define _DVS_AUDIO_ARBITER_H_

struct dvs_engine; /* forward declaration; full type in engine/engine_ctx.h */

/* Route audioOwner -> ALSA playback device and volume -> mixer (req 03 §7.3).
 *
 * owner: logical audio owner. If it starts with "hw:" it is treated as a
 *        concrete ALSA device string; otherwise "default" is used. Empty owner
 *        means "no audio" -> muted (volume forced to 0).
 * volume: 0..100 linear percentage applied to the ALSA "Master" element. */
void dvs_audio_set(struct dvs_engine* e, const char* owner, int volume);

#endif /* _DVS_AUDIO_ARBITER_H_ */
