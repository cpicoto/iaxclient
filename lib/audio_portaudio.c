/*
 * iaxclient: a cross-platform IAX softphone library
 *
 * Copyrights:
 * Copyright (C) 2003-2006, Horizon Wimba, Inc.
 * Copyright (C) 2007, Wimba, Inc.
 *
 * Contributors:
 * Steve Kann <stevek@stevek.com>
 * Michael Van Donselaar <mvand@vandonselaar.org>
 * Shawn Lawrence <shawn.lawrence@terracecomm.com>
 * Erik Bunce <kde@bunce.us>
 *
 * This program is free software, distributed under the terms of
 * the GNU Lesser (Library) General Public License.
 *
 * Module: audio_portaudio
 * Purpose: Audio code to provide portaudio driver support for IAX library
 * Developed by: Shawn Lawrence, Terrace Communications Inc.
 * Creation Date: April 18, 2003
 *
 * This library uses the PortAudio Portable Audio Library
 * For more information see: http://www.portaudio.com/
 * PortAudio Copyright (c) 1999-2000 Ross Bencina and Phil Burk
 *
 */


#if defined(WIN32)  ||  defined(_WIN32_WCE)
#include <stdlib.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include <pa_ringbuffer.h>
#include "audio_portaudio.h"
#include "iaxclient_lib.h"
#include "portmixer.h"
#include <pa_win_wasapi.h>    /* for PaWasapiStreamInfo */
#include <speex/speex_resampler.h> // Add Speex resampler header

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #define PORT_LOG(fmt, ...)                                                    \
    do {                                                                           \
      char _buf[512];                                                              \
      char _time_buf[32];                                                          \
      SYSTEMTIME _st;                                                              \
      GetLocalTime(&_st);                                                          \
      snprintf(_time_buf, sizeof(_time_buf), "%02d:%02d:%02d.%03d",                \
               _st.wHour, _st.wMinute, _st.wSecond, _st.wMilliseconds);            \
      _snprintf(_buf, sizeof(_buf), "%s:[portaudio-debug] " fmt "\n",                 \
               _time_buf, ##__VA_ARGS__);                                          \
      OutputDebugStringA(_buf);                                                    \
    } while(0)
#else
  #include <time.h>
  #include <sys/time.h>
  #define PORT_LOG(fmt, ...)                                                    \
    do {                                                                           \
      struct timeval tv;                                                           \
      struct tm* tm_info;                                                          \
      char _time_buf[32];                                                          \
      gettimeofday(&tv, NULL);                                                     \
      tm_info = localtime(&tv.tv_sec);                                             \
      strftime(_time_buf, sizeof(_time_buf), "%H:%M:%S", tm_info);                 \
      char _ms_buf[8];                                                             \
      snprintf(_ms_buf, sizeof(_ms_buf), ".%03d", (int)(tv.tv_usec / 1000));       \
      strcat(_time_buf, _ms_buf);                                                  \
      fprintf(stderr, "[portaudio-debug %s] " fmt "\n", _time_buf, ##__VA_ARGS__);    \
    } while(0)
#endif


#ifdef USE_MEC2
#define DO_EC
#include "mec3.h"
static echo_can_state_t *ec;
#endif

#ifdef SPAN_EC
#define DO_EC
#include "ec/echo.h"
static echo_can_state_t *ec;
#endif

#if defined(SPEEX_EC) && ! defined (WIN32)
#define DO_EC
#define restrict __restrict
#include "speex/speex_echo.h"
static SpeexEchoState *ec;
#endif

#define EC_RING_SZ  8192 /* must be pow(2) */


typedef short SAMPLE;
// Add this to the static variables section (around line 100, after sample_rate declaration)

static int mixers_initialized;
static float input_level = 1.0f;  // Add this line for the missing variable

static PaStream *iStream, *oStream, *aStream;
static PxMixer *iMixer = NULL, *oMixer = NULL;

static int selectedInput, selectedOutput, selectedRing;

static int sample_rate = 8000;
static int mixers_initialized;
static int startup_counter = 0;  // Add this line

static int current_audio_format = 0;  // Add this to track audio format
static int pa_openwasapi(struct iaxc_audio_driver *d); //AD7NP move to wasapi
/* actual WASAPI host rate and ratio for resampling */
static double host_sample_rate = 0.0;  /* card’s native rate, e.g. 48000.0 */
static double sample_ratio     = 1.0;  /* host_sample_rate / internal sample_rate (8000) */
static SpeexResamplerState *speex_resampler = NULL;
static SpeexResamplerState *output_resampler = NULL;

static int last_output_buf_size = 0;
static int output_underruns = 0;
static int output_samples_played = 0;

#define MAX_SAMPLE_RATE       48000
#ifndef MS_PER_FRAME
# define MS_PER_FRAME         40
#endif
#define SAMPLES_PER_FRAME     (MS_PER_FRAME * sample_rate / 1000)

/* static frame buffer allocation */
#define MAX_SAMPLES_PER_FRAME (MS_PER_FRAME * MAX_SAMPLE_RATE  / 1000)

/* echo_tail length, in frames must be pow(2) for mec/span ? */
#define ECHO_TAIL 4096

/* RingBuffer Size; Needs to be Pow(2), 1024 = 512 samples = 64ms */
#ifndef OUTRBSZ
# define OUTRBSZ (32768)
#endif

/* Input ringbuffer size;  this doesn't seem to be as critical, and making it big
 * causes issues when we're answering calls, etc., and the audio system is running
 * but not being drained */
#ifndef INRBSZ
# define INRBSZ  (16384)
#endif

/* TUNING:  The following constants may help in tuning for situations
 * where you are getting audio-level under/overruns.
 *
 * If you are running iaxclient on a system where you cannot get
 * low-latency scheduling, you may need to increase these.  This tends
 * to be an issue on non-MacOSX unix systems, when you are not running
 * as root, and cannot ask the OS for higher priority.
 *
 * RBOUTTARGET:  This a target size of the output ringbuffer, in milliseconds,
 * where audio for your speakers goes after being decoded and mixed, and
 * before the audio callback asks for it.  It can get larger than this
 * (up to OUTRBSZ, above), but when it does, for a bit, we will start
 * dropping some frames.  For no drops at all, this needs to be set to
 * contain the number of samples in your largest scheduling gap
 *
 * PA_NUMBUFFERS:  This is the number of buffers that the low-level
 * operating system driver will use, for buffering our output (and also
 * our input) between the soundcard and portaudio.  This should also be
 * set to the maximum scheduling delay.  Some drivers, though, will
 * callback _into_ portaudio with a higher priority, so this doesn't
 * necessarily need to be as big as RBOUTMAXSZ, although on linux, it
 * does.  The default is to leave this up to portaudio..
 */

/* 80ms if average outRing length is more than this many bytes, start dropping */
#ifndef RBOUTTARGET
# define RBOUTTARGET (30)
#endif

/* size in bytes of ringbuffer target */
#define RBOUTTARGET_BYTES (RBOUTTARGET * (sample_rate / 1000) * sizeof(SAMPLE))

static char inRingBuf[INRBSZ*sizeof(SAMPLE)];
static char outRingBuf[OUTRBSZ*sizeof(SAMPLE)];
static PaUtilRingBuffer inRing, outRing;

static int outRingLenAvg;

static int oneStream;
static int auxStream;
static int virtualMonoIn;
static int virtualMonoOut;
static int virtualMonoRing;

static int running;

static struct iaxc_sound *sounds;
static int  nextSoundId = 1;

static MUTEX sound_lock;

/* forward declarations */
static int pa_start (struct iaxc_audio_driver *d );
static void handle_paerror(PaError err, char * where);
static int pa_input_level_set(struct iaxc_audio_driver *d, float level);
static float pa_input_level_get(struct iaxc_audio_driver *d);
static int pa_output_level_set(struct iaxc_audio_driver *d, float level);

/* scan devices and stash pointers to dev structures.
 *  But, these structures only remain valid while Pa is initialized,
 *  which, with pablio, is only while it's running!
 *  Also, storing these things in two separate arrays loses the actual
 *  PaDeviceID's associated with devices (since their index in these
 *  input/output arrays isn't the same as their index in the combined
 *  array */
static int scan_devices(struct iaxc_audio_driver *d)
{
	int nDevices;
	int i;

	d->nDevices = nDevices = Pa_GetDeviceCount();
	d->devices = (struct iaxc_audio_device *)
		malloc(nDevices * sizeof(struct iaxc_audio_device));

	for ( i=0; i < nDevices; i++ )
	{
		const PaDeviceInfo *pa;
		struct iaxc_audio_device *dev;

		pa=Pa_GetDeviceInfo(i);
		dev = &(d->devices[i]);

		if ( pa ) //frik: under Terminal Services this is NULL
		{
			dev->name = (char *)pa->name;
			dev->devID = i;
			dev->capabilities = 0;

			if ( pa->maxInputChannels > 0 ){
				dev->capabilities |= IAXC_AD_INPUT;
				PORT_LOG("scan_devices:IAXC_AD_INPUT: %s", dev->name);
			}

			if ( pa->maxOutputChannels > 0 )
			{
				dev->capabilities |= IAXC_AD_OUTPUT;
				dev->capabilities |= IAXC_AD_RING;
				PORT_LOG("scan_devices:IAXC_AD_OUTPUT: %s", dev->name);
			}

			if ( i == Pa_GetDefaultInputDevice() ){
				dev->capabilities |= IAXC_AD_INPUT_DEFAULT;
				PORT_LOG("scan_devices:IAXC_AD_INPUT_DEFAULT: %s", dev->name);
			}
			if ( i == Pa_GetDefaultOutputDevice() )
			{
				dev->capabilities |= IAXC_AD_OUTPUT_DEFAULT;
				dev->capabilities |= IAXC_AD_RING_DEFAULT;
				PORT_LOG("scan_devices:IAXC_AD_OUTPUT_DEFAULT: %s", dev->name);
			}
		}
		else //frik: under Terminal Services
		{
			dev->name = "Not usable device";
			dev->devID = i;
			dev->capabilities = 0;
		}
	}

	return 0;
}

static void mono2stereo(SAMPLE *out, SAMPLE *in, int nSamples)
{
	int i;
	//fprintf(stderr, "mono2stereo: %d samples\n", nSamples);
	for ( i=0; i < nSamples; i++ )
	{
		*(out++) = *in;
		*(out++) = *(in++);
	}
}

static void stereo2mono(SAMPLE *out, SAMPLE *in, int nSamples)
{
	int i;
	//fprintf(stderr, "stereo2mono: %d samples\n", nSamples);
	for ( i=0; i < nSamples; i++ )
	{
		*(out) = *(in++);
		out++; in++;
		//*(out++) += *(in++);
	}
}

static void mix_slin(short *dst, short *src, int samples, int virtualMono)
{
	int i=0,val=0;
	for ( i=0; i < samples; i++ )
	{
		if ( virtualMono )
			val = ((short *)dst)[2*i] + ((short *)src)[i];
		else
			val = ((short *)dst)[i] + ((short *)src)[i];

		if ( val > 0x7fff )
		{
			val = 0x7fff-1;
		} else if (val < -0x7fff)
		{
			val = -0x7fff+1;
		}

		if ( virtualMono )
		{
			dst[2*i] = val;
			dst[2*i+1] = val;
		} else
		{
			dst[i] = val;
		}

	}
}

static int pa_mix_sounds (void *outputBuffer, unsigned long frames, int channel, int virtualMono)
{
	struct iaxc_sound *s;
	struct iaxc_sound **sp;
	unsigned long outpos;

	MUTEXLOCK(&sound_lock);
	/* mix each sound into the outputBuffer */
	sp = &sounds;
	while ( sp && *sp )
	{
		s = *sp;
		outpos = 0;

		if ( s->channel == channel )
		{
			/* loop over the sound until we've played it enough
			 * times, or we've filled the outputBuffer */
			for(;;)
			{
				int n;

				if ( outpos == frames )
					break;  /* we've filled the buffer */
				if ( s->pos == s->len )
				{
					if ( s->repeat == 0 )
					{
						// XXX free the sound
						// structure, and maybe the
						// buffer!
						(*sp) = s->next;
						if(s->malloced)
							free(s->data);
						free(s);
						break;
					}
					s->pos = 0;
					s->repeat--;
				}

				/* how many frames do we add in this loop? */
				n = (frames - outpos) < (unsigned long)(s->len - s->pos) ?
					(frames - outpos) :
					(unsigned long)(s->len - s->pos);

				/* mix in the frames */
				mix_slin((short *)outputBuffer + outpos,
						s->data+s->pos, n, virtualMono);

				s->pos += n;
				outpos += n;
			}
		}
		if ( *sp ) /* don't advance if we removed this member */
			sp = &((*sp)->next);
	}
	MUTEXUNLOCK(&sound_lock);
	return 0;
}

static int pa_play_sound(struct iaxc_sound *inSound, int ring)
{
	struct iaxc_sound *sound;

	sound = (struct iaxc_sound *)malloc(sizeof(struct iaxc_sound));
	if ( !sound )
		return 1;

	*sound = *inSound;

	MUTEXLOCK(&sound_lock);
	sound->channel = ring;
	sound->id = nextSoundId++;
	sound->pos = 0;

	sound->next = sounds;
	sounds = sound;
	MUTEXUNLOCK(&sound_lock);

	if ( !running )
		pa_start(NULL); /* XXX fixme: start/stop semantics */

	return sound->id;
}

static int pa_stop_sound(int soundID)
{
	struct iaxc_sound **sp;
	int retval = 1; /* not found */

	MUTEXLOCK(&sound_lock);
	for ( sp = &sounds; *sp; (*sp) = (*sp)->next )
	{
		struct iaxc_sound *s = *sp;
		if ( s->id == soundID )
		{
			if ( s->malloced )
				free(s->data);
			/* remove from list */
			(*sp) = s->next;
			free(s);

			retval= 0; /* found */
			break;
		}
	}
	MUTEXUNLOCK(&sound_lock);

	return retval; /* found? */
}

static void iaxc_echo_can(short *inputBuffer, short *outputBuffer, int n)
{
	static PaUtilRingBuffer ecOutRing;
	static char outRingBuf[EC_RING_SZ];
	static long bias = 0;
	short  delayedBuf[1024];
	int i;

	/* remove bias -- whether ec is on or not. */
	for ( i = 0; i < n; i++ )
	{
		bias += ((((long int) inputBuffer[i]) << 15) - bias) >> 14;
		inputBuffer[i] -= (short int) (bias >> 15);
	}

	/* if ec is off, clear ec state -- this way, we start fresh if/when
	 * it's turned back on. */
	if ( !(iaxc_get_filters() & IAXC_FILTER_ECHO) )
	{
#if defined(DO_EC)
		if ( ec )
		{
#if defined(USE_MEC2) || defined(SPAN_EC)
			echo_can_free(ec);
			ec = NULL;
#elif defined(SPEEX_EC)
			speex_echo_state_destroy(ec);
			ec = NULL;
#endif
		}
#endif

		return;
	}

	/* we want echo cancellation */

#if defined(DO_EC)
	if ( !ec )
	{
		PaUtil_InitializeRingBuffer(&ecOutRing, 1, EC_RING_SZ,  &outRingBuf);
#if defined(USE_MEC2) || defined(SPAN_EC)
		ec = echo_can_create(ECHO_TAIL, 0);
#elif defined(SPEEX_EC)
		ec = speex_echo_state_init(SAMPLES_PER_FRAME, ECHO_TAIL);
#endif
	}
#endif

	/* fill ecOutRing */
//	PaUtil_WriteRingBuffer(&ecOutRing, outputBuffer, n * 2);
	PaUtil_WriteRingBuffer(&ecOutRing, outputBuffer, n * 2);

	// Make sure we have enough buffer.
	// Currently, just one SAMPLES_PER_FRAME's worth.
	if ( PaUtil_GetRingBufferWriteAvailable(&ecOutRing) < ((n + SAMPLES_PER_FRAME) * 2) )
		return;

	PaUtil_ReadRingBuffer(&ecOutRing, delayedBuf, n * 2);

#if defined(DO_EC) && defined(SPEEX_EC)
	{
		short cancelledBuffer[1024];

		speex_echo_cancel(ec, inputBuffer, delayedBuf,
				cancelledBuffer, NULL);

		for ( i = 0; i < n; i++ )
			inputBuffer[i] = cancelledBuffer[i];
	}
#endif

#if defined(USE_MEC2) || defined(SPAN_EC)
	for ( i = 0; i < n; i++ )
		inputBuffer[i] = echo_can_update(ec, delayedBuf[i],
				inputBuffer[i]);
#endif
}

// Debug function to verify audio data is present
static void debug_check_output_audio(const SAMPLE* buf, int len) {
    static int zero_frames = 0;
    static int last_report = 0;
    static int total_frames = 0;
    
    if (!buf || len <= 0) return;
    
    total_frames++;
    
    // Check if the frame contains only zeros
    int all_zeros = 1;
    for (int i = 0; i < len && i < 20; i++) {
        if (buf[i] != 0) {
            all_zeros = 0;
            break;
        }
    }
    
    if (all_zeros) {
        zero_frames++;
    } else {
        zero_frames = 0; // Reset when we get real audio
    }
    
    // Report every 200 frames or when we have 50 consecutive zero frames
    if (total_frames - last_report > 200 || zero_frames > 50) {
        if (all_zeros) {
            PORT_LOG("debug_check_output_audio:OUTPUT AUDIO: %d consecutive silent frames", zero_frames);
        } else {
            PORT_LOG("debug_check_output_audio:OUTPUT AUDIO: Active audio data detected [%d, %d, %d, %d]", 
                    buf[0], buf[1], buf[2], buf[3]);
        }
        last_report = total_frames;
    }
}


static int pa_callback(
    const void                    *inputBuffer,
          void                    *outputBuffer,
    unsigned long                  hostFrames,
    const PaStreamCallbackTimeInfo *timeInfo,
    PaStreamCallbackFlags          statusFlags,
    void                          *userData
){
    const SAMPLE *inBuf = (const SAMPLE*)inputBuffer;
    SAMPLE *outBuf = (SAMPLE*)outputBuffer;
    static int debug_counter = 0;

    if (!iMixer && input_level != 1.0f && inputBuffer) {
        short *samples = (short*)inputBuffer;
        for (unsigned long i = 0; i < hostFrames; i++) {  // Use hostFrames instead of framesPerBuffer
            samples[i] = (short)(samples[i] * input_level);
        }
    }
    // Skip processing if no input buffer but still clear output buffer
    if (!inputBuffer) {
        if (outputBuffer) {
            memset(outBuf, 0, hostFrames * sizeof(SAMPLE));
        }
        return paContinue;
    }
    
    // *** INPUT PROCESSING (CAPTURE) ***
    // Use Speex resampler if we have different sample rates
    if (speex_resampler && host_sample_rate > sample_rate) {
        // Create buffer for resampled output
        static SAMPLE resampled_buffer[2048]; // Buffer for resampled output
        
        // Calculate the number of output samples we expect based on the ratio
        spx_uint32_t in_len = hostFrames;
        spx_uint32_t out_len = (spx_uint32_t)(hostFrames / sample_ratio) + 1;
        
        // Make sure we don't exceed our buffer size
        if (out_len > 2048) {
            out_len = 2048;
        }
        
        // Process audio through the resampler
        int err = speex_resampler_process_int(
            speex_resampler,
            0, // Channel index (0 for mono)
            inBuf,
            &in_len,
            resampled_buffer,
            &out_len
        );
        
        // Write resampled audio to the ring buffer
        int written = PaUtil_WriteRingBuffer(&inRing, resampled_buffer, out_len);
#ifdef VERBOSE        
        if (debug_counter % 500 == 0) {
            PORT_LOG("PA_CALLBACK: Resampled %lu frames to %lu frames (%d written to buffer)",
                    hostFrames, (unsigned long)out_len, written);
        }
#endif

    } else {
        // If no resampling needed or no resampler available, use direct copy
        PaUtil_WriteRingBuffer(&inRing, inBuf, hostFrames);
    }
    
    // *** OUTPUT PROCESSING (PLAYBACK) ***
    if (outputBuffer) {
        if (output_resampler && host_sample_rate > sample_rate) {
            // Calculate how many 8kHz samples we need for the required hostFrames
            int samples_needed = (int)(hostFrames / sample_ratio);
            
            // Intermediate buffer for 8kHz audio
            static SAMPLE buffer_8k[1024]; 
            
            // Check how many samples are available in the output ring buffer
            int available = PaUtil_GetRingBufferReadAvailable(&outRing);
            
            if (available > 0) {
                // Read up to samples_needed or what's available
                int samples_to_read = (available > samples_needed) ? samples_needed : available;
                
                // Read 8kHz data from ring buffer
                int actually_read = PaUtil_ReadRingBuffer(&outRing, buffer_8k, samples_to_read);
                
                if (actually_read > 0) {
                    // Check for audio content (debugging only)
                    if (debug_counter % 500 == 0) {
                        int has_audio = 0;
                        int max_value = 0;
                        
                        for (int i = 0; i < actually_read && i < 50; i++) {
                            if (buffer_8k[i] != 0) {
                                has_audio = 1;
                                int abs_val = abs(buffer_8k[i]);
                                if (abs_val > max_value) max_value = abs_val;
                            }
                        }
#ifdef VERBOSE
                        if (has_audio) {
                            PORT_LOG("pa_callback:OUTPUT AUDIO: Active audio data, max amplitude: %d", max_value);
                        }
#endif
                    }
                    
                    // Resample from 8kHz to host_sample_rate
                    spx_uint32_t in_len = actually_read;
                    spx_uint32_t out_len = hostFrames;
                    
                    int err = speex_resampler_process_int(
                        output_resampler,
                        0,             // Channel index
                        buffer_8k,     // Input buffer (8kHz)
                        &in_len,       // Input samples
                        outBuf,        // Output buffer (48kHz)
                        &out_len       // Output samples
                    );
                    
                    // If we didn't fill the whole buffer, fill with silence
                    if (out_len < hostFrames) {
                        memset(outBuf + out_len, 0, (hostFrames - out_len) * sizeof(SAMPLE));
                    }
                    
                    output_samples_played += actually_read;
                } else {
                    // No samples read, output silence
                    memset(outBuf, 0, hostFrames * sizeof(SAMPLE));
                }
            } else {
                // No samples available
                output_underruns++;
                if (output_underruns % 100 == 0) {
                    PORT_LOG("pa_callback:OUTPUT UNDERRUN (%d): No data available for audio output", 
                            output_underruns);
                }
                memset(outBuf, 0, hostFrames * sizeof(SAMPLE));
            }
        } else {
            // Legacy path for when no resampler is available
            int samples_read = 0;
            memset(outBuf, 0, hostFrames * sizeof(SAMPLE));
            
            for (unsigned long j = 0; j < hostFrames; ++j) {
                SAMPLE outS = 0;
                if (j % (int)sample_ratio == 0) {
                    if (PaUtil_ReadRingBuffer(&outRing, &outS, 1) == 1) {
                        samples_read++;
                    }
                }
                outBuf[j] = outS;
            }
            
            if (debug_counter % 500 == 0) {
                PORT_LOG("pa_callback:LEGACY OUTPUT: Read %d samples for %lu frames", 
                        samples_read, hostFrames);
            }
        }
    }
    
    debug_counter++;
    return paContinue;
}


static int pa_aux_callback(void *inputBuffer, void *outputBuffer,
	    unsigned long samplesPerFrame,
	    const PaStreamCallbackTimeInfo* outTime,
	    PaStreamCallbackFlags statusFlags,
	    void *userData)
{
	int totBytes = samplesPerFrame * sizeof(SAMPLE) * (virtualMonoRing + 1);

	if ( outputBuffer )
	{
		memset((char *)outputBuffer, 0, totBytes);
		pa_mix_sounds(outputBuffer, samplesPerFrame, 1, virtualMonoRing);
	}
	return 0;
}

static int pa_open(int single, int inMono, int outMono)
{
	PaError err;
	PaDeviceInfo *result;

	
    PORT_LOG("pa_open: single=%d, inMono=%d, outMono=%d", single, inMono, outMono);
    PORT_LOG("pa_open: selectedInput=%d, selectedOutput=%d", selectedInput, selectedOutput);

	struct PaStreamParameters in_stream_params, out_stream_params, no_device;
	in_stream_params.device = selectedInput;
	in_stream_params.channelCount = (inMono ? 1 : 2);
	in_stream_params.sampleFormat = paInt16;
    PORT_LOG("pa_open:Input stream format explicitly set to 0x%x (paInt16)", paInt16);
	result = (PaDeviceInfo *)Pa_GetDeviceInfo(selectedInput);
	if ( result == NULL ) return -1;
	in_stream_params.suggestedLatency = result->defaultLowInputLatency;
	in_stream_params.hostApiSpecificStreamInfo = NULL;

	out_stream_params.device = selectedOutput;
	out_stream_params.channelCount = (outMono ? 1 : 2);
	out_stream_params.sampleFormat = paInt16;
	result = (PaDeviceInfo *)Pa_GetDeviceInfo(selectedOutput);
	if ( result == NULL ) return -1;
	out_stream_params.suggestedLatency = result->defaultLowOutputLatency;
	out_stream_params.hostApiSpecificStreamInfo = NULL;

	no_device.device = paNoDevice;
	no_device.channelCount = 0;
	no_device.sampleFormat = paInt16;
	result = (PaDeviceInfo *)Pa_GetDeviceInfo(selectedInput);
	if ( result == NULL ) return -1;
	no_device.suggestedLatency = result->defaultLowInputLatency; // FEEDBACK - unsure if appropriate
	no_device.hostApiSpecificStreamInfo = NULL;

	if ( single )
	{
		err = Pa_OpenStream(&iStream,
			&in_stream_params,
			&out_stream_params,
			sample_rate,
			paFramesPerBufferUnspecified, //FEEBACK - unsure if appropriate
			paNoFlag,
			(PaStreamCallback *)pa_callback,
			NULL);
		if (err != paNoError) return -1;
		oStream = iStream;
		oneStream = 1;
	} else
	{
		err = Pa_OpenStream(&iStream,
			&in_stream_params,
			&no_device,
			sample_rate,
			paFramesPerBufferUnspecified, //FEEBACK - unsure if appropriate
			paNoFlag,
			(PaStreamCallback *)pa_callback,
			NULL);
		if ( err != paNoError ) return -1;

		err = Pa_OpenStream(&oStream,
			&no_device,
			&out_stream_params,
			sample_rate,
			paFramesPerBufferUnspecified, //FEEBACK - unsure if appropriate
			paNoFlag,
			(PaStreamCallback *)pa_callback,
			NULL);

		if ( err != paNoError )
		{
			Pa_CloseStream(iStream);
			iStream = NULL;
			return -1;
		}
		oneStream = 0;
	}

	virtualMonoIn = (inMono ? 0 : 1);
	virtualMonoOut = (outMono ? 0 : 1);
	return 0;
}

/* some commentary here:
 * 1: MacOSX: MacOSX often needs "virtual mono" and a single stream.
 * That doesn't work for some USB devices (a Platronics headset), so
 * mono in, virtual mono out, and mono in/out are also tried.
 *
 * 2: Unix/OSS: most cards are OK with real mono, and a single stream.
 * Except some.  For those, a single open with real mono will succeed,
 * but execution will fail.  Maybe others will open OK with a single
 * stream, and real mono, but fail later? Two stream mono is tried first,
 * since it reportedly provides better sound quality with ALSA
 * and Sound Blaster Live.
 *
 * The failure mode I saw with a volunteer was that reads/writes would
 * return -enodev (down in the portaudio code).  Bummer.
 *
 * Win32 works fine, in all cases, with a single stream and real mono,
 * so far.
 *
 * We could probably do this more cleanly, because there are still cases
 * where we will fail (i.e. if the user has only mono in and out on a Mac).
 *
 * */
static int pa_openstreams (struct iaxc_audio_driver *d )
{
	int err;
#ifdef _WIN32
    /* On Windows, use our WASAPI helper: */
    return pa_openwasapi(d);
#else
    /* On other platforms, fall back to the old PortAudio open: */
    return pa_open(d);
#endif
#ifdef LINUX
	err = pa_open(0, 1, 1) && /* two stream mono */
		pa_open(1, 1, 1) &&   /* one stream mono */
		pa_open(0, 0, 0);     /* two stream stereo */
#else
#ifdef MACOSX
	err = pa_open(1, 0, 0) &&  /* one stream stereo */
		pa_open(1, 1, 0) &&    /* one stream mono in stereo out */
		pa_open(1, 1, 1) &&    /* one stream mono */
		pa_open(0, 0, 0);      /* two stream stereo */
#else
	err = pa_open(1, 1, 1) &&  /* one stream mono */
		pa_open(1, 0, 0) &&    /* one stream stereo */
		pa_open(1, 1, 0) &&    /* one stream mono in stereo out */
		pa_open(0, 0, 0);      /* two stream stereo */
#endif /*MACOSX */
#endif /* LINUX */

	if (err)
	{
		handle_paerror(err, "Unable to open streams");
		PORT_LOG("pa_openstreams:Unable to open streams: %d", err);
		return -1;
	}
	return 0;
}
/*---------------------------------------------------------------------------*/
static int pa_openwasapi(struct iaxc_audio_driver *d)
{
    PaError err;
    
    // 1) Find WASAPI host API
    PaHostApiIndex apiIndex = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (apiIndex < 0) {
        PORT_LOG("pa_openwasapi: WASAPI not available");
        return -1;
    }

    // 2) Get default WASAPI devices (or use selected ones if specified)
    const PaHostApiInfo *apiInfo = Pa_GetHostApiInfo(apiIndex);
    PaDeviceIndex inDev, outDev;
    
    // Use selected devices if they're valid for WASAPI
    if (selectedInput >= 0 && Pa_GetHostApiInfo(Pa_GetDeviceInfo(selectedInput)->hostApi)->type == paWASAPI) {
        inDev = selectedInput;
    } else {
        inDev = Pa_HostApiDeviceIndexToDeviceIndex(apiIndex, apiInfo->defaultInputDevice);
    }
    
    if (selectedOutput >= 0 && Pa_GetHostApiInfo(Pa_GetDeviceInfo(selectedOutput)->hostApi)->type == paWASAPI) {
        outDev = selectedOutput;
    } else {
        outDev = Pa_HostApiDeviceIndexToDeviceIndex(apiIndex, apiInfo->defaultOutputDevice);
    }
    
    if (inDev < 0 || outDev < 0) {
        PORT_LOG("pa_openwasapi: no WASAPI I/O device");
        return -1;
    }

    // 3) Compute host rate and resample ratio
    host_sample_rate = Pa_GetDeviceInfo(inDev)->defaultSampleRate;
    sample_ratio = host_sample_rate / (double)sample_rate;
    PORT_LOG("pa_openwasapi: Requested WASAPI to deliver audio at %.1fHz (native rate) with resampling to %d Hz", 
            host_sample_rate, sample_rate);

    // Clean up existing resamplers
    if (speex_resampler) {
        speex_resampler_destroy(speex_resampler);
        speex_resampler = NULL;
    }
    
    if (output_resampler) {
        speex_resampler_destroy(output_resampler);
        output_resampler = NULL;
    }
    
    // Only create resampler if sample rates differ
    if (host_sample_rate != sample_rate) {
        int err_in = 0;  // Initialize these variables
        int err_out = 0;
        
        // Create input resampler (48k → 8k)
        speex_resampler = speex_resampler_init(
            1,                              // 1 channel (mono)
            (spx_uint32_t)host_sample_rate, // Input rate (48000)
            (spx_uint32_t)sample_rate,      // Output rate (8000)
            5,                              // Quality (0-10, 5 is good quality)
            &err_in
        );
        
        // Create output resampler (8k → 48k)
        output_resampler = speex_resampler_init(
            1,                              // 1 channel (mono)
            (spx_uint32_t)sample_rate,      // Input rate (8000)
            (spx_uint32_t)host_sample_rate, // Output rate (48000)
            5,                              // Quality (0-10, 5 is good quality)
            &err_out
        );
        
        if (err_in != RESAMPLER_ERR_SUCCESS || err_out != RESAMPLER_ERR_SUCCESS) {
            PORT_LOG("pa_openwasapi:Failed to initialize Speex resamplers: in=%s, out=%s", 
                    speex_resampler_strerror(err_in),
                    speex_resampler_strerror(err_out));
        } else {
            PORT_LOG("pa_openwasapi:Speex resampler initialized: %d Hz ↔ %d Hz", 
                    (int)host_sample_rate, sample_rate);
        }
    }

    // Fix zero audio format issue
    if (current_audio_format == 0) {
        current_audio_format = paInt16;
        PORT_LOG("pa_openwasapi: Fixed zero format, using paInt16");
    }

    // 4) Fill WASAPI-specific info
    PaWasapiStreamInfo wasapiInfo = {
        .size           = sizeof(PaWasapiStreamInfo),
        .hostApiType    = paWASAPI,
        .version        = 1,
        .flags          = paWinWasapiAutoConvert | paWinWasapiThreadPriority,
        .threadPriority = eThreadPriorityProAudio,
        .channelMask    = PAWIN_SPEAKER_FRONT_CENTER,  // mono
        .streamCategory = eAudioCategoryCommunications,
        .streamOption   = eStreamOptionNone
    };

    // 5) Fill your PortAudio parameters
    PaStreamParameters inParams = {
        .device                    = inDev,
        .channelCount              = 1,
        .sampleFormat              = current_audio_format,
        .suggestedLatency          = Pa_GetDeviceInfo(inDev)->defaultLowInputLatency,
        .hostApiSpecificStreamInfo = &wasapiInfo
    };
    PaStreamParameters outParams = {
        .device                    = outDev,
        .channelCount              = 1,
        .sampleFormat              = current_audio_format,
        .suggestedLatency          = Pa_GetDeviceInfo(outDev)->defaultLowOutputLatency,
        .hostApiSpecificStreamInfo = &wasapiInfo
    };

    // 6) Open full-duplex stream at host_sample_rate
    err = Pa_OpenStream(
        &iStream,
        &inParams,
        &outParams,
        host_sample_rate,  // Use native 48kHz rate
        1024,              // Explicit buffer size helps with stability
        paNoFlag,
        pa_callback,       // Callback is already set here
        d
    );
    
    if (err != paNoError) {
        PORT_LOG("pa_openwasapi: Pa_OpenStream failed: %s", Pa_GetErrorText(err));
        return -1;
    }

    // 7) Use one stream for both in/out
    oneStream = 1;
    oStream = iStream;
    
    return 0;
}
/*---------------------------------------------------------------------------*/

static int pa_openauxstream (struct iaxc_audio_driver *d )
{
	PaError err;

	struct PaStreamParameters ring_stream_params;

	// setup the ring parameters
	ring_stream_params.device = selectedRing;
	ring_stream_params.sampleFormat = paInt16;
	ring_stream_params.suggestedLatency =
		Pa_GetDeviceInfo(selectedRing)->defaultLowOutputLatency;
	ring_stream_params.hostApiSpecificStreamInfo = NULL;

	// first we'll try mono
	ring_stream_params.channelCount = 1;

	err = Pa_OpenStream(&aStream,
			NULL,
			&ring_stream_params,
			sample_rate,
			paFramesPerBufferUnspecified, //FEEBACK - unsure if appropriate
			paNoFlag,
			(PaStreamCallback *)pa_aux_callback,
			NULL);

	if ( err != paNoError )
	{
		// next we'll try virtual mono (stereo)
		ring_stream_params.channelCount = 1;

		err = Pa_OpenStream(&aStream,
				NULL,
				&ring_stream_params,
				sample_rate,
				paFramesPerBufferUnspecified, //FEEBACK - unsure if appropriate
				paNoFlag,
				(PaStreamCallback *)pa_aux_callback,
				NULL);
	}

	// mmok, failure...
	if ( err != paNoError )
	{
		// fprintf(stderr, "Failure opening ring device with params: id: %d, output %d, default output %d\n",
		// selectedRing, selectedOutput, Pa_GetDefaultOutputDevice());

		handle_paerror(err, "opening separate ring stream");
		return -1;
	}

	// Determine whether virtual mono is being used
	virtualMonoRing = ring_stream_params.channelCount - 1;

	return 0;
}

static int pa_start(struct iaxc_audio_driver *d)
{
	static int errcnt = 0;
	current_audio_format = paInt16;  // Fix for 0x0 format issue


	if ( running )
		return 0;

	PORT_LOG("pa_start: format to 0x%x (paInt16)", current_audio_format);
	// Add format check
	if (d != NULL) {
		PORT_LOG("pa_start:Audio format before start: 0x%x %s", 
			current_audio_format, 
			(current_audio_format == 0) ? "INVALID!" : "ok");
		
		// Fix zero format if needed
		if (current_audio_format == 0) {
			current_audio_format = paInt16;
			PORT_LOG("pa_start:Fixed zero format to 0x%x (paInt16)", current_audio_format);
		}
	}
	/* re-open mixers if necessary */
	if ( iMixer )
	{
		Px_CloseMixer(iMixer);
		iMixer = NULL;
	}

	if ( oMixer )
	{
		Px_CloseMixer(oMixer);
		oMixer = NULL;
	}

	if ( errcnt > 5 )
	{
		iaxci_usermsg(IAXC_TEXT_TYPE_FATALERROR,
				"iaxclient audio: Can't open Audio Device. "
				"Perhaps you do not have an input or output device?");
		/* OK, we'll give the application the option to abort or
		 * not here, but we will throw a fatal error anyway */
		PORT_LOG("pa_start:Unable to open audio device after 5 attempts. Giving up.");
		iaxc_millisleep(1000);
		//return -1; // Give Up.  Too many errors.
	}

	/* flush the ringbuffers */
	PaUtil_InitializeRingBuffer(&inRing, sizeof(SAMPLE), INRBSZ, inRingBuf);
	PaUtil_InitializeRingBuffer(&outRing, sizeof(SAMPLE), OUTRBSZ, outRingBuf);

	if ( pa_openstreams(d) )
	{
		errcnt++;
		return -1;
	}

	errcnt = 0; // only count consecutive errors.

	if ( Pa_StartStream(iStream) != paNoError )
		return -1;

	iMixer = Px_OpenMixer(iStream, 0);

	if ( !oneStream )
	{
		PaError err = Pa_StartStream(oStream);
		oMixer = Px_OpenMixer(oStream, 0);
		if ( err != paNoError )
		{
			Pa_StopStream(iStream);
			return -1;
		}
	}

	if ( selectedRing != selectedOutput )
	{
		auxStream = 1;
	}
	else
	{
		auxStream = 0;
	}

	if ( auxStream )
	{
		pa_openauxstream(d);
		if ( Pa_StartStream(aStream) != paNoError )
		{
			auxStream = 0;
		}
	}

	/* select the microphone as the input source */
	if ( iMixer != NULL && !mixers_initialized )
	{
		/* First, select the "microphone" device, if it's available */
		/* try the new method, reverting to the old if it fails */
		if ( Px_SetCurrentInputSourceByName( iMixer, "microphone" ) != 0 )
		{
			int n = Px_GetNumInputSources( iMixer ) - 1;
			for ( ; n > 0; --n )
			{
				if ( !strcasecmp("microphone",
						Px_GetInputSourceName(iMixer, n)) )
				{
					Px_SetCurrentInputSource( iMixer, n );
					PORT_LOG("pa_start:Using microphone input source %d", n);
				}
			}
		}

		/* try to set the microphone boost -- we just turn off this
		 * "boost" feature, because it often leads to clipping, which
		 * we can't fix later -- but we can deal with low input levels
		 * much more gracefully */
		Px_SetMicrophoneBoost( iMixer, 0 );

		/* If the input level is very low, raise it up a bit.
		 * Otherwise, AGC cannot detect speech, and cannot adjust
		 * levels */
		if ( pa_input_level_get(d) < 0.5f )
			pa_input_level_set(d, 0.6f);
		mixers_initialized = 1;
	}
    PORT_LOG("pa_start:Streams started successfully");
	running = 1;
	return 0;
}

static int pa_stop (struct iaxc_audio_driver *d )
{
	PaError err;

	if ( !running )
		return 0;

	if ( sounds )
		return 0;

	err = Pa_AbortStream(iStream);
	err = Pa_CloseStream(iStream);

	if ( !oneStream )
	{
		err = Pa_AbortStream(oStream);
		err = Pa_CloseStream(oStream);
	}

	if ( auxStream )
	{
		err = Pa_AbortStream(aStream);
		err = Pa_CloseStream(aStream);
	}

	running = 0;
	return 0;
}

/* Mihai: apparently nobody loves this function. Some actually hate it.
 * I bet if it's gone, no one will miss it.  Such a cold, cold world!
static void pa_shutdown()
{
	CloseAudioStream( iStream );
	if(!oneStream) CloseAudioStream( oStream );
	if(auxStream) CloseAudioStream( aStream );
}
*/

static void handle_paerror(PaError err, char * where)
{
	fprintf(stderr, "PortAudio error at %s: %s\n", where,
			Pa_GetErrorText(err));
}

static int pa_input(struct iaxc_audio_driver *d, void *samples, int *nSamples)
{
    static int error_count = 0;
    static int last_success_time = 0;
    static int call_count = 0;
    int elementsToRead = *nSamples;
    int available = PaUtil_GetRingBufferReadAvailable(&inRing);
#ifdef VERBOSE    
    // Reduce log spam by only logging every few calls
    if (call_count++ % 20 == 0) {
        PORT_LOG("pa_input: Available=%d elements, requested=%d", available, elementsToRead);
    }
#endif    
    if (available < elementsToRead) {
        // Return partial data if we have more than half requested
        if (available > elementsToRead/2) {
#ifdef VERBOSE			
            PORT_LOG("pa_input: Returning partial data (%d elements)", available);
#endif			
            PaUtil_ReadRingBuffer(&inRing, samples, available);
            *nSamples = available;
            error_count = 0;
            return 0;
        }
        
        // During startup or after silence
        if (startup_counter++ < 200) {
            *nSamples = 0;
            return 0;  // Return success with 0 samples
        }
        
        // Add small delay to allow buffer to fill if we're getting errors
        if (++error_count > 5) {
            error_count = 0;
            iaxc_millisleep(5);  // Small sleep to allow buffer to fill
        }
        
        *nSamples = 0;
        return 1;  // Error - not enough data
    }
    
    // Success - we have enough data
    PaUtil_ReadRingBuffer(&inRing, samples, elementsToRead);
    error_count = 0;
    startup_counter = 0;  // Reset startup counter on success
    return 0;
}

static int pa_output(struct iaxc_audio_driver *d, void *samples, int nSamples)
{
    // Get ring buffer metrics
    int outRingLen = PaUtil_GetRingBufferWriteAvailable(&outRing);
    outRingLenAvg = (outRingLenAvg * 9 + outRingLen) / 10;
    
    // Simple buffer management - only drop if absolutely necessary
    if (outRingLen < nSamples) {
        // This is a buffer overflow situation - log it
        PORT_LOG("OUTPUT DROP: Buffer overflow (%d samples needed, only %d available)", 
                nSamples, outRingLen);
        return 0;
    }
    
    // Write the data to the ring buffer
    int written = PaUtil_WriteRingBuffer(&outRing, samples, nSamples);
    
    // Log any partial writes
    if (written < nSamples) {
        PORT_LOG("OUTPUT DROP: Could only write %d of %d samples", written, nSamples);
    }
    
    return written;
}

static int pa_select_devices(struct iaxc_audio_driver *d, int input,
		int output, int ring)
{
	selectedInput = input;
	selectedOutput = output;
	selectedRing = ring;
	if ( running )
	{
		/* stop/start audio, in order to switch devices */
		pa_stop(d);
		pa_start(d);
	}
	else
	{
		/* start/stop audio, in order to initialize mixers and levels */
		pa_start(d);
		pa_stop(d);
	}
	return 0;
}

static int pa_selected_devices(struct iaxc_audio_driver *d, int *input,
		int *output, int *ring)
{
	*input = selectedInput;
	*output = selectedOutput;
	*ring = selectedRing;
	return 0;
}


static int pa_destroy(struct iaxc_audio_driver *d)
{
	// Clean up Speex resampler if it was created
	if (speex_resampler) {
		speex_resampler_destroy(speex_resampler);
		speex_resampler = NULL;
	}
	if (output_resampler) {
		speex_resampler_destroy(output_resampler);
		output_resampler = NULL;
	}
	if( iMixer )
	{
		Px_CloseMixer(iMixer);
		iMixer = NULL;
	}
	if ( oMixer )
	{
		Px_CloseMixer(oMixer);
		oMixer = NULL;
	}
	if ( d )
	{
		if ( d->devices )
		{
			free(d->devices);
			d->devices= NULL;
		}
	}
	return Pa_Terminate();
}

static float pa_input_level_get(struct iaxc_audio_driver *d)
{
	/* iMixer should be non-null if we using either one or two streams */
	if ( !iMixer )
		return -1;

	/* make sure this device supports input volume controls */
	if ( Px_GetNumInputSources( iMixer ) == 0 )
		return -1;

	return Px_GetInputVolume(iMixer);
}

static float pa_output_level_get(struct iaxc_audio_driver *d)
{
	PxMixer *mix;

	/* oMixer may be null if we're using one stream,
	   in which case, iMixer should not be null,
	   if it is, return an error */

	if ( oMixer )
		mix = oMixer;
	else if ( iMixer )
		mix = iMixer;
	else
		return -1;

	/* prefer the pcm output, but default to the master output */
	if ( Px_SupportsPCMOutputVolume(mix) )
		return Px_GetPCMOutputVolume(mix);
	else
		return Px_GetMasterVolume(mix);
}

static int pa_input_level_set(struct iaxc_audio_driver *d, float level)
{
    PORT_LOG("pa_input_level_set: Setting input level to %f", level);
    
    // Check if mixer is available
    if (iMixer) {
        // Hardware mixer volume control
        PORT_LOG("pa_input_level_set: Using hardware mixer control");
        Px_SetInputVolume(iMixer, level);
        return 0;
    } else {
        // Software gain adjustment
        PORT_LOG("pa_input_level_set: No hardware mixer available, using software gain");
        input_level = level;  // Store for software gain application
        return 0;
    }
}

static int pa_output_level_set(struct iaxc_audio_driver *d, float level)
{
	PxMixer *mix;

	if ( oMixer )
		mix = oMixer;
	else if ( iMixer )
		mix = iMixer;
	else
		return -1;

	/* prefer the pcm output, but default to the master output */
	if ( Px_SupportsPCMOutputVolume(mix) )
		Px_SetPCMOutputVolume(mix, level);
	else
		Px_SetMasterVolume(mix, level);

	return 0;
}

static int pa_mic_boost_get(struct iaxc_audio_driver* d)
{
	if ( !iMixer )
		return -1;

	return Px_GetMicrophoneBoost(iMixer);
}

int pa_mic_boost_set(struct iaxc_audio_driver* d, int enable)
{
	if ( !iMixer )
		return -1;

	return Px_SetMicrophoneBoost(iMixer, enable);
}

/* initialize audio driver */
static int _pa_initialize (struct iaxc_audio_driver *d, int sr)
{
	PaError  err;

	sample_rate = sr;
#ifdef VERBOSE
    PORT_LOG("_pa_initialize:Initializing PortAudio with sample rate %d", sr);
#endif
	/* initialize portaudio */
	if ( paNoError != (err = Pa_Initialize()) )
	{
		PORT_LOG("_pa_initialize:Pa_Initialize failed with error %d: %s", 
			err, Pa_GetErrorText(err));
		iaxci_usermsg(IAXC_TEXT_TYPE_ERROR, "Failed Pa_Initialize");
		return err;
	}
#ifdef VERBOSE
    PORT_LOG("_pa_initialize:Pa_Initialize succeeded, scanning devices");
#endif
	scan_devices(d);
#ifdef VERBOSE
	PORT_LOG("_pa_initialize:Found %d audio devices", d->nDevices);
#endif

	/* setup methods */
	d->initialize = pa_initialize;
	d->destroy = pa_destroy;
	d->select_devices = pa_select_devices;
	d->selected_devices = pa_selected_devices;
	d->start = pa_start;
	d->stop = pa_stop;
	d->output = pa_output;
	d->input = pa_input;
	d->input_level_get = pa_input_level_get;
	d->input_level_set = pa_input_level_set;
	d->output_level_get = pa_output_level_get;
	d->output_level_set = pa_output_level_set;
	d->play_sound = pa_play_sound;
	d->stop_sound = pa_stop_sound;
	d->mic_boost_get = pa_mic_boost_get;
	d->mic_boost_set = pa_mic_boost_set;

	/* setup private data stuff */
	selectedInput  = Pa_GetDefaultInputDevice();
	selectedOutput = Pa_GetDefaultOutputDevice();
	selectedRing   = Pa_GetDefaultOutputDevice();
	sounds = NULL;
	MUTEXINIT(&sound_lock);

	PaUtil_InitializeRingBuffer(&inRing, sizeof(SAMPLE), INRBSZ, inRingBuf);
	PaUtil_InitializeRingBuffer(&outRing, sizeof(SAMPLE), OUTRBSZ, outRingBuf);
	// Initialize the ring buffers properly
	PaUtil_FlushRingBuffer(&inRing);
	PaUtil_FlushRingBuffer(&outRing);

	// Add some silence at the beginning to prime the buffer
	SAMPLE silence[160] = {0};
	PaUtil_WriteRingBuffer(&outRing, silence, 160);
#ifdef VERBOSE
	PORT_LOG("_pa_initialize:Ring buffers initialized (in: %d, out: %d)",

		PaUtil_GetRingBufferReadAvailable(&inRing),
		PaUtil_GetRingBufferReadAvailable(&outRing));
#endif
	running = 0;
#ifdef VERBOSE
    PORT_LOG("_pa_initialize:PortAudio initialization complete");
#endif
	return 0;
}

/* standard initialization:  Do the normal initialization, and then
   also initialize mixers and levels */
int pa_initialize(struct iaxc_audio_driver *d, int sr)
{
	PORT_LOG("pa_initialize: Setting up audio driver with sample rate %d", sr);
	_pa_initialize(d, sr);
    current_audio_format = paInt16;  // Fix for 0x0 format issue
    PORT_LOG("pa_initialize(2): Explicitly setting audio format to 0x%x (paInt16)", current_audio_format);

	/* TODO: Kludge alert. We only do the funny audio start-stop
	 * business if iaxci_audio_output_mode is not set. This is a
	 * hack to allow certain specific users of iaxclient to avoid
	 * certain problems associated with portaudio initialization
	 * hitting a deadlock condition.
	 */
	if ( iaxci_audio_output_mode )
		return 0;

	/* start/stop audio, in order to initialize mixers and levels */
	pa_start(d);
	pa_stop(d);

	return 0;
}

/* alternate initialization:  delay mixer/level initialization until
   we actually start the device.  This is somewhat useful when you're about to start
   the device as soon as you've initialized it, and want to avoid the time it
   takes to start/stop the device before starting it again */
int pa_initialize_deferred(struct iaxc_audio_driver *d, int sr)
{
	_pa_initialize(d, sr);
	return 0;
}

