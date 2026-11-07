#ifndef _DVS_AUDIO_ARBITER_H_
#define _DVS_AUDIO_ARBITER_H_

struct dvs_engine; /* forward declaration; full type in engine/engine_ctx.h */

#include <stddef.h>
#include <stdint.h>

/* Route audioOwner (window identifier) -> lane audio subscription + volume.
 *
 * owner: window identifier selecting the lane that carries audio. Empty means
 *        no audio (mute, volume forced to 0). Accepted forms: "lane<N>",
 *        decimal lane index, "sip:port", or exact "sip". Anything else fails
 *        with no fallback to the main lane. volume: 0..100 Master percentage.
 * Applied state (owner/sip/port/volume/ok) is tracked on the engine so query
 * reflects what actually took effect, never the pending request. */
void dvs_audio_set(struct dvs_engine* e, const char* owner, int volume);

/* Resolve owner against the running compositor lanes. 0 ok, -1 no match. */
int dvs_audio_resolve(const struct dvs_engine* e, const char* owner,
                      char* sip_out, size_t sip_len, uint16_t* port_out);

#endif /* _DVS_AUDIO_ARBITER_H_ */
