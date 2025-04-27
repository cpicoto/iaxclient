// audio_openal.h
#ifndef _AUDIO_OPENAL_H
#define _AUDIO_OPENAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "iaxclient_lib.h"  // for struct iaxc_audio_driver & struct iaxc_sound

// Opaque backend state
struct openal_priv_data;

/**
 * Initialize the OpenAL audio backend.
 * @param d           the iaxc_audio_driver to fill–in
 * @param sample_rate the audio sample rate (e.g. 8000)
 * @return  0 on success, non-zero on error
 */
int  openal_initialize(struct iaxc_audio_driver *d, int sample_rate);

/**
 * Shutdown and free the OpenAL backend.
 * @param d  the iaxc_audio_driver to destroy
 */
int  openal_destroy(struct iaxc_audio_driver *d);

/**
 * (No-op) Tell the backend which devices to use.
 * @return  0 success, -1 error
 */
int  openal_select_devices(struct iaxc_audio_driver *d, int input, int output, int ring);
int  openal_selected_devices(struct iaxc_audio_driver *d, int *input, int *output, int *ring);

/**
 * Start/stop audio I/O.  IAXClient may call start() multiple times.
 */
int  openal_start(struct iaxc_audio_driver *d);
int  openal_stop (struct iaxc_audio_driver *d);

/**
 * Pull raw PCM in (mono 16-bit) from hardware.
 * @param samples  buffer to fill with 16-bit signed samples
 * @param nSamples on entry = max frames, on exit = frames returned
 * @return  0 success, 1 silence (but keeps in sync), non-zero error
 */
int  openal_input (struct iaxc_audio_driver *d, void *samples, int *nSamples);

/**
 * Push raw PCM out (mono 16-bit) to hardware.
 * @param samples  buffer of 16-bit signed samples
 * @param nSamples number of frames in that buffer
 * @return  0 success, non-zero on underrun/overflow
 */
int  openal_output(struct iaxc_audio_driver *d, void *samples, int nSamples);

/**
 * Get/set per-stream levels.
 */
float openal_input_level_get (struct iaxc_audio_driver *d);
int   openal_input_level_set (struct iaxc_audio_driver *d, float level);
float openal_output_level_get(struct iaxc_audio_driver *d);
int   openal_output_level_set(struct iaxc_audio_driver *d, float level);

/**
 * Sound API stubs (no-op under OpenAL backend).
 */
int  openal_play_sound(int id, int ring);
int  openal_stop_sound(int id);

/**
 * Microphone boost (no-op under OpenAL backend).
 */
int  openal_mic_boost_get(struct iaxc_audio_driver *d);
int  openal_mic_boost_set(struct iaxc_audio_driver *d, int enable);

#ifdef __cplusplus
}
#endif

#endif // _AUDIO_OPENAL_H
