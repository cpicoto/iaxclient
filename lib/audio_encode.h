/*
 * iaxclient: a cross-platform IAX softphone library
 */

#ifndef _AUDIO_ENCODE_H
#define _AUDIO_ENCODE_H

/* Minimum dB possible in the iaxclient world. This level
 * is intended to represent silence.
 */
#define AUDIO_ENCODE_SILENCE_DB -99.0f

// Make sure EXPORT is defined before using it
#ifndef EXPORT
#define EXPORT extern
#endif

struct iaxc_call;
struct iax_event;

int audio_send_encoded_audio(struct iaxc_call * most_recent_answer, int callNo,
        void * data, int iEncodeType, int samples);

int audio_decode_audio(struct iaxc_call * p, void * out, void * data, int len,
        int iEncodeType, int * samples);

/* Audio capture functions for debugging */
EXPORT void iaxc_debug_audio_capture_start(void);
EXPORT void iaxc_debug_audio_capture_stop(void);
EXPORT void iaxc_ptt_audio_capture_start(void);
EXPORT void iaxc_ptt_audio_capture_stop(void);
EXPORT void iaxc_handle_audio_event(const char* message);

#endif

