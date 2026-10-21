/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ALSA PCM playback wrapper for RX audio output.
 */

#ifndef _ALSA_AUDIO_H_
#define _ALSA_AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

/** Opaque ALSA playback context. */
typedef struct alsa_audio alsa_audio;

/**
 * Open ALSA playback device.
 * @param device  ALSA device string (e.g. "default", "hw:0,0")
 * @param out     [out] receives context
 * @return 0 on success
 */
int alsa_audio_open(const char* device, alsa_audio** out);

/**
 * Write PCM data to the playback device (blocking).
 * @param a       context
 * @param data    interleaved S16_LE samples
 * @param frames  number of frames (samples per channel)
 * @return 0 on success, -EPIPE on underrun (auto-recovered)
 */
int alsa_audio_write(alsa_audio* a, const int16_t* data, unsigned long frames);

/**
 * Get the current playback position in seconds.
 * Used as the master clock for A/V sync.
 */
double alsa_audio_clock_get(const alsa_audio* a);

/** Close the ALSA device and free context. */
void alsa_audio_close(alsa_audio* a);

#endif /* _ALSA_AUDIO_H_ */
