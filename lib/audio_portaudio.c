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
#include "pa_ringbuffer_extensions.h"
#include "audio_portaudio.h"
#include "iaxclient_lib.h"
#include "portmixer.h"
#include <pa_win_wasapi.h>    /* for PaWasapiStreamInfo */
#include <speex/speex_resampler.h> // Add Speex resampler header

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <initguid.h>  // Required for COM GUID definitions
  #include <mmdeviceapi.h> // For MMDevice interfaces
  #include <audiopolicy.h> // For audio session interfaces
  #include <functiondiscoverykeys_devpkey.h>
  #include <avrt.h>     // For MMCSS thread priority
  #define PORT_LOG(fmt, ...)\
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
# ifdef _WIN32
#  define OUTRBSZ (131072)  /* Much larger output buffer for Windows stability (128K) */
# else
#  define OUTRBSZ (32768)
# endif
#endif

/* Input ringbuffer size;  this doesn't seem to be as critical, and making it big
 * causes issues when we're answering calls, etc., and the audio system is running
 * but not being drained */
#ifndef INRBSZ
# ifdef _WIN32
#  define INRBSZ  (65536)  /* Larger input buffer for Windows buffering (64K) */
# else
#  define INRBSZ  (16384)
# endif
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
# define RBOUTTARGET (80)  /* Optimized for low latency while preventing underruns */
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
static int error_count;
static int startup_counter;
static int output_underruns;
static HANDLE healthCheckThread;
static volatile LONG healthCheckThreadActive; // Use volatile LONG for atomic operations

static struct iaxc_sound *sounds;
static int  nextSoundId = 1;

static MUTEX sound_lock;

/* forward declarations */
static int pa_start (struct iaxc_audio_driver *d );
static void handle_paerror(PaError err, char * where);
static int pa_input_level_set(struct iaxc_audio_driver *d, float level);
static float pa_input_level_get(struct iaxc_audio_driver *d);
static int pa_output_level_set(struct iaxc_audio_driver *d, float level);
static BOOL check_exclusive_mode_support(const PaDeviceInfo* deviceInfo);
static double find_supported_wasapi_exclusive_rate(const PaDeviceInfo* inDevInfo, const PaDeviceInfo* outDevInfo);
static void pa_boost_buffer(void);
#ifdef _WIN32
static DWORD WINAPI HealthCheckTimerThread(LPVOID param);
static void pa_setup_windows_audio_session(void);
static CRITICAL_SECTION pa_stream_lock;
static int pa_stream_lock_initialized = 0;

// Compatibility function for InterlockedRead (not available on all Windows versions)
static LONG InterlockedRead(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}
#endif

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
#ifdef VERBOSE                
				PORT_LOG("scan_devices:IAXC_AD_INPUT: %s", dev->name);
#endif                
			}

			if ( pa->maxOutputChannels > 0 )
			{
				dev->capabilities |= IAXC_AD_OUTPUT;
				dev->capabilities |= IAXC_AD_RING;
#ifdef VERBOSE                                
				PORT_LOG("scan_devices:IAXC_AD_OUTPUT: %s", dev->name);
#endif                
			}

			if ( i == Pa_GetDefaultInputDevice() ){	
#ifdef VERBOSE                                
				PORT_LOG("scan_devices:IAXC_AD_INPUT_DEFAULT: %s", dev->name);
#endif                
			}
			if ( i == Pa_GetDefaultOutputDevice() )
			{
				dev->capabilities |= IAXC_AD_OUTPUT_DEFAULT;
				dev->capabilities |= IAXC_AD_RING_DEFAULT;
#ifdef VERBOSE                                
				PORT_LOG("scan_devices:IAXC_AD_OUTPUT_DEFAULT: %s", dev->name);
#endif
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

	// Reset underrun counters when starting to play a sound
	// This helps prevent unnecessary stream resets while playing sounds
	output_underruns = 0;
	error_count = 0;
	
	// Add extra buffer boost when starting sound playback
	// to prevent underruns at the beginning
	pa_boost_buffer();

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
    static int consecutive_underruns = 0;
    static int64_t total_frames_processed = 0;
    
    // Track total frames for diagnostics
    total_frames_processed += hostFrames;
    
    // Check for xruns using statusFlags
    if (statusFlags & paInputUnderflow) {
        PORT_LOG("pa_callback: INPUT UNDERFLOW detected at frame %lld", 
                (long long)total_frames_processed);
    }
    if (statusFlags & paInputOverflow) {
        PORT_LOG("pa_callback: INPUT OVERFLOW detected at frame %lld", 
                (long long)total_frames_processed);
    }
    if (statusFlags & paOutputUnderflow) {
        if (++consecutive_underruns % 10 == 0) {
            PORT_LOG("pa_callback: Multiple OUTPUT UNDERFLOWS detected (%d in a row) at frame %lld", 
                    consecutive_underruns, (long long)total_frames_processed);
            
            // If we're having serious underruns, add some buffer to help stabilize
            if (consecutive_underruns >= 50 && outputBuffer) {
                // Try to compensate by adding silence to output buffer as a fallback
                static SAMPLE silence[2048] = {0};
                PaUtil_WriteRingBuffer(&outRing, silence, sizeof(silence)/sizeof(SAMPLE));
                PORT_LOG("pa_callback: Added extra silence buffer to stabilize playback");
            }
        }
    } else {
        consecutive_underruns = 0;
    }
    if (statusFlags & paOutputOverflow) {
        PORT_LOG("pa_callback: OUTPUT OVERFLOW detected");
    }

    // Apply software input gain if no hardware mixer available
    if (!iMixer && input_level != 1.0f && inputBuffer) {
        short *samples = (short*)inputBuffer;
        for (unsigned long i = 0; i < hostFrames; i++) {
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
        static SAMPLE resampled_buffer[4096]; // Larger buffer for higher quality resampling
        
        // Calculate the number of output samples we expect based on the ratio
        spx_uint32_t in_len = hostFrames;
        spx_uint32_t out_len = (spx_uint32_t)(hostFrames / sample_ratio) + 1;
        
        // Make sure we don't exceed our buffer size
        if (out_len > 4096) {
            out_len = 4096;
        }
        
        // Process audio through the resampler
        int err = speex_resampler_process_int(
            speex_resampler,
            0,      // Channel index (0 for mono)
            inBuf,  // Input buffer (host rate)
            &in_len,
            resampled_buffer,  // Output buffer (8kHz)
            &out_len
        );
        
        if (err != RESAMPLER_ERR_SUCCESS) {
            PORT_LOG("pa_callback: Resampling error: %s", speex_resampler_strerror(err));
        }
        
        // Write resampled audio to the ring buffer with overflow protection
        int ring_space = PaUtil_GetRingBufferWriteAvailable(&inRing);
        if (ring_space < out_len) {
            PORT_LOG("pa_callback: Input ring buffer overflow! Available=%d, Needed=%d", 
                    ring_space, (int)out_len);
            // Still write what we can
            out_len = ring_space;
        }
        
        int written = PaUtil_WriteRingBuffer(&inRing, resampled_buffer, out_len);
#ifdef VERBOSE        
        if (debug_counter % 500 == 0) {
            PORT_LOG("pa_callback: Resampled %lu frames to %lu frames (%d written to buffer)",
                    hostFrames, (unsigned long)out_len, written);
        }
#endif        
    } else {
        // If no resampling needed or no resampler available, use direct copy
        int ring_space = PaUtil_GetRingBufferWriteAvailable(&inRing);
        if (ring_space < hostFrames) {
            PORT_LOG("pa_callback: Input ring buffer overflow! Available=%d, Needed=%lu", 
                    ring_space, hostFrames);
            // Still write what we can
            PaUtil_WriteRingBuffer(&inRing, inBuf, ring_space);
        } else {
            PaUtil_WriteRingBuffer(&inRing, inBuf, hostFrames);
        }
    }
    
    // *** OUTPUT PROCESSING (PLAYBACK) ***
    if (outputBuffer) {
        if (output_resampler && host_sample_rate > sample_rate) {
            // Calculate how many 8kHz samples we need for the required hostFrames
            int samples_needed = (int)(hostFrames / sample_ratio);
            
            // Intermediate buffer for 8kHz audio
            static SAMPLE buffer_8k[2048]; // Increased buffer size for better quality
            
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

                        if (has_audio) {
                            PORT_LOG("pa_callback: OUTPUT AUDIO: Active audio data, max amplitude: %d", max_value);
                        }
                    }
                    
                    // Resample from 8kHz to host_sample_rate with better quality settings
                    spx_uint32_t in_len = actually_read;
                    spx_uint32_t out_len = hostFrames;
                    
                    int err = speex_resampler_process_int(
                        output_resampler,
                        0,             // Channel index
                        buffer_8k,     // Input buffer (8kHz)
                        &in_len,       // Input samples
                        outBuf,        // Output buffer (host rate)
                        &out_len       // Output samples
                    );
                    
                    if (err != RESAMPLER_ERR_SUCCESS) {
                        PORT_LOG("pa_callback: Output resampling error: %s", speex_resampler_strerror(err));
                    }
                    
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
#ifdef VERBOSE                    
                    PORT_LOG("pa_callback: OUTPUT UNDERRUN (%d): No data available for audio output", 
                            output_underruns);
#endif
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
                PORT_LOG("pa_callback: LEGACY OUTPUT: Read %d samples for %lu frames", 
                        samples_read, hostFrames);
            }
        }    }
    
    debug_counter++;
    
    // Perform periodic health checks during callback
    static DWORD last_health_time = 0;
    DWORD current_time = GetTickCount();
    
    // Check health every 30 seconds from the audio thread
    if (current_time - last_health_time > 30000) {
        last_health_time = current_time;
        
        // Check for issues that might indicate problems
        if (consecutive_underruns > 100 || output_underruns > 1000) {
            PORT_LOG("pa_callback: Audio performance issues detected, may require recovery");
            // Reset counters so we don't trigger too often
            consecutive_underruns = 0;
            output_underruns = 0;
            
            // Actual recovery is performed in pa_input/pa_output calls
        }
#ifdef VERBOSE        
        // Log performance stats periodically
        PORT_LOG("pa_callback: HEALTH CHECK - %lld frames processed, %d underruns, %d overflows",
                (long long)total_frames_processed, output_underruns, 
                PaUtil_GetRingBufferFullCount(&inRing));
#endif
    }
    
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
	static int wasapi_failures = 0;
	
#ifdef _WIN32
    /* On Windows, try WASAPI first, but fall back if it keeps failing */
    if (wasapi_failures < 3) {
        err = pa_openwasapi(d);
        if (err == 0) {
            PORT_LOG("pa_openstreams: WASAPI opened successfully");
            wasapi_failures = 0; // Reset failure count on success
            return 0;
        } else {
            wasapi_failures++;
            PORT_LOG("pa_openstreams: WASAPI failed (attempt %d/3), trying fallback", wasapi_failures);
            if (wasapi_failures >= 3) {
                PORT_LOG("pa_openstreams: WASAPI disabled due to repeated failures");
            }
        }
    }
    
    /* Fall back to regular PortAudio */
    PORT_LOG("pa_openstreams: Using regular PortAudio fallback");
    return pa_open(0, 1, 1);
#else
    /* On other platforms, use regular PortAudio: */
    return pa_open(0, 1, 1);
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
/* Checks if a device supports exclusive mode with WASAPI */
static BOOL check_exclusive_mode_support(const PaDeviceInfo* deviceInfo)
{
    if (!deviceInfo) {
        return FALSE;
    }
    
    // Get device info from PortAudio
    const PaHostApiInfo* apiInfo = Pa_GetHostApiInfo(deviceInfo->hostApi);
    if (!apiInfo || apiInfo->type != paWASAPI) {
        return FALSE;
    }
    
    // Be more conservative about exclusive mode - only try it for high-end devices
    if (strstr(deviceInfo->name, "ASIO") || 
        strstr(deviceInfo->name, "Studio") ||
        strstr(deviceInfo->name, "Professional") ||
        strstr(deviceInfo->name, "Audio Interface")) {
        PORT_LOG("check_exclusive_mode_support: Device '%s' looks like pro audio interface", deviceInfo->name);
        return TRUE;
    }
    
    // For regular consumer devices, skip exclusive mode to avoid compatibility issues
    PORT_LOG("check_exclusive_mode_support: Device '%s' - using shared mode for compatibility", deviceInfo->name);
    return FALSE;
}

/*---------------------------------------------------------------------------*/
static double find_supported_wasapi_exclusive_rate(const PaDeviceInfo* inDevInfo, const PaDeviceInfo* outDevInfo)
{
    if (!inDevInfo || !outDevInfo) {
        PORT_LOG("find_supported_wasapi_exclusive_rate: Device info not available");
        return 0.0;
    }
    
    // Check if exclusive mode is supported on both devices
    BOOL inExclusive = check_exclusive_mode_support(inDevInfo);
    BOOL outExclusive = check_exclusive_mode_support(outDevInfo);
    
    if (!inExclusive || !outExclusive) {
        PORT_LOG("find_supported_wasapi_exclusive_rate: Exclusive mode not supported on %s", 
                !inExclusive ? "input device" : "output device");
        return 0.0;
    }
    
    // Common sample rates to try, from highest quality to lowest
    static const double rates[] = {
        192000.0,
        176400.0,
        96000.0,
        88200.0,
        48000.0,
        44100.0,
        32000.0,
        22050.0,
        16000.0,
        11025.0,
        8000.0
    };
    
    // Default to the device's default sample rate if available
    double defaultRate = outDevInfo->defaultSampleRate;
    if (defaultRate > 0) {
        return defaultRate;
    }
    
    // Return standard rate that works well with most devices
    return 48000.0;
}

static int pa_openwasapi(struct iaxc_audio_driver *d)
{
    PaError err;
    HANDLE mmcssHandle = NULL;
    DWORD taskIndex = 0;
    
    // 1) Find WASAPI host API
    PaHostApiIndex apiIndex = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (apiIndex < 0) {
        PORT_LOG("pa_openwasapi: WASAPI not available, falling back to default PortAudio");
        return pa_open(0, 1, 1); // Fall back to regular PortAudio
    }

    // 2) Get default WASAPI devices (or use selected ones if specified)
    const PaHostApiInfo *apiInfo = Pa_GetHostApiInfo(apiIndex);
    if (!apiInfo || apiInfo->deviceCount <= 0) {
        PORT_LOG("pa_openwasapi: No WASAPI devices available, falling back to default PortAudio");
        return pa_open(0, 1, 1);
    }
    
    PaDeviceIndex inDev = paNoDevice, outDev = paNoDevice;
    
    // Use selected devices if they're valid for WASAPI
    if (selectedInput >= 0) {
        const PaDeviceInfo* inputInfo = Pa_GetDeviceInfo(selectedInput);
        if (inputInfo && inputInfo->hostApi == apiIndex) {
            inDev = selectedInput;
            PORT_LOG("pa_openwasapi: Using selected input device: %s", inputInfo->name);
        }
    }
    
    if (selectedOutput >= 0) {
        const PaDeviceInfo* outputInfo = Pa_GetDeviceInfo(selectedOutput);
        if (outputInfo && outputInfo->hostApi == apiIndex) {
            outDev = selectedOutput;
            PORT_LOG("pa_openwasapi: Using selected output device: %s", outputInfo->name);
        }
    }
    
    // Fall back to default devices if not using selected ones
    if (inDev == paNoDevice && apiInfo->defaultInputDevice >= 0) {
        inDev = Pa_HostApiDeviceIndexToDeviceIndex(apiIndex, apiInfo->defaultInputDevice);
    }
    if (outDev == paNoDevice && apiInfo->defaultOutputDevice >= 0) {
        outDev = Pa_HostApiDeviceIndexToDeviceIndex(apiIndex, apiInfo->defaultOutputDevice);
    }
    
    // Final validation of devices
    if (inDev == paNoDevice || outDev == paNoDevice) {
        PORT_LOG("pa_openwasapi: No valid WASAPI devices found (in=%d, out=%d), falling back to default PortAudio", inDev, outDev);
        return pa_open(0, 1, 1); // Fall back to regular PortAudio
    }    // 3) Get device info and validate devices are actually available
    const PaDeviceInfo* inDevInfo = Pa_GetDeviceInfo(inDev);
    const PaDeviceInfo* outDevInfo = Pa_GetDeviceInfo(outDev);
    
    if (!inDevInfo || !outDevInfo) {
        PORT_LOG("pa_openwasapi: Could not get device info, falling back to default PortAudio");
        return pa_open(0, 1, 1);
    }
    
    // Check that devices support the required properties
    if (inDevInfo->maxInputChannels < 1 || outDevInfo->maxOutputChannels < 1) {
        PORT_LOG("pa_openwasapi: Devices don't support required channels (in=%d, out=%d), falling back to default PortAudio", 
                inDevInfo->maxInputChannels, outDevInfo->maxOutputChannels);
        return pa_open(0, 1, 1);
    }
    
    PORT_LOG("pa_openwasapi: Selected devices - Input: '%s', Output: '%s'", 
            inDevInfo->name, outDevInfo->name);
    
    // 4) Determine safe sample rate
    host_sample_rate = 48000.0; // Start with a safe common rate
    
    // Use device's default sample rate if it's reasonable
    if (outDevInfo->defaultSampleRate >= 8000.0 && outDevInfo->defaultSampleRate <= 192000.0) {
        host_sample_rate = outDevInfo->defaultSampleRate;
        PORT_LOG("pa_openwasapi: Using device's default sample rate: %.1fHz", host_sample_rate);
    } else {
        PORT_LOG("pa_openwasapi: Device sample rate %.1fHz out of range, using 48kHz", outDevInfo->defaultSampleRate);
    }
    
    sample_ratio = host_sample_rate / (double)sample_rate;
    PORT_LOG("pa_openwasapi: WASAPI will use native rate %.1fHz with resampling to/from %d Hz", 
            host_sample_rate, sample_rate);    // 5) Set up resamplers if needed
    if (speex_resampler) {
        speex_resampler_destroy(speex_resampler);
        speex_resampler = NULL;
    }
    
    if (output_resampler) {
        speex_resampler_destroy(output_resampler);
        output_resampler = NULL;
    }
    
    // Only create resampler if sample rates differ significantly
    if (fabs(host_sample_rate - sample_rate) > 0.1) {
        int err_in = 0, err_out = 0;
        
        speex_resampler = speex_resampler_init(
            1, (spx_uint32_t)host_sample_rate, (spx_uint32_t)sample_rate, 6, &err_in);
        
        output_resampler = speex_resampler_init(
            1, (spx_uint32_t)sample_rate, (spx_uint32_t)host_sample_rate, 6, &err_out);
        
        if (err_in != RESAMPLER_ERR_SUCCESS || err_out != RESAMPLER_ERR_SUCCESS) {
            PORT_LOG("pa_openwasapi: Failed to initialize Speex resamplers, falling back to default PortAudio");
            return pa_open(0, 1, 1);
        }
    }

    // 6) Fix audio format if needed
    if (current_audio_format == 0 || current_audio_format == paCustomFormat) {
        current_audio_format = paInt16;
        PORT_LOG("pa_openwasapi: Fixed invalid format, using paInt16");
    }

    // 7) Try to boost thread priority (optional - don't fail if this doesn't work)
    mmcssHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (mmcssHandle) {
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
        PORT_LOG("pa_openwasapi: Set thread to MMCSS Pro Audio class");
    }

    // 8) Configure WASAPI for shared mode (most compatible)
    PaWasapiStreamInfo wasapiInfo = {
        .size           = sizeof(PaWasapiStreamInfo),
        .hostApiType    = paWASAPI,
        .version        = 1,
        .flags          = paWinWasapiAutoConvert,
        .threadPriority = eThreadPriorityAudio,
        .streamCategory = eAudioCategoryCommunications,
        .streamOption   = eStreamOptionNone
    };

    // 9) Configure conservative stream parameters
    PaStreamParameters inParams = {
        .device                    = inDev,
        .channelCount              = 1,
        .sampleFormat              = paInt16,  // Force known good format
        .suggestedLatency          = 0.05,     // Conservative 50ms latency
        .hostApiSpecificStreamInfo = &wasapiInfo
    };

    PaStreamParameters outParams = {
        .device                    = outDev,
        .channelCount              = 1,
        .sampleFormat              = paInt16,  // Force known good format
        .suggestedLatency          = 0.05,     // Conservative 50ms latency
        .hostApiSpecificStreamInfo = &wasapiInfo
    };

    // 10) Try multiple approaches to open the stream
    PORT_LOG("pa_openwasapi: Attempting to open WASAPI stream (devices: %d->%d, rate=%.1fHz)", 
            inDev, outDev, host_sample_rate);
    
    // First attempt: Conservative shared mode
    err = Pa_OpenStream(
        &iStream,
        &inParams,
        &outParams,
        host_sample_rate,
        paFramesPerBufferUnspecified,
        paNoFlag,
        pa_callback,
        d
    );
    
    if (err != paNoError) {
        PORT_LOG("pa_openwasapi: Conservative mode failed: %s (0x%x)", Pa_GetErrorText(err), err);
        
        // Second attempt: Try with device's recommended latency
        inParams.suggestedLatency = inDevInfo->defaultLowInputLatency;
        outParams.suggestedLatency = outDevInfo->defaultLowOutputLatency;
        
        err = Pa_OpenStream(
            &iStream,
            &inParams,
            &outParams,
            host_sample_rate,
            paFramesPerBufferUnspecified,
            paNoFlag,
            pa_callback,
            d
        );
        
        if (err != paNoError) {
            PORT_LOG("pa_openwasapi: Recommended latency failed: %s (0x%x)", Pa_GetErrorText(err), err);
            
            // Third attempt: High latency mode
            inParams.suggestedLatency = inDevInfo->defaultHighInputLatency;
            outParams.suggestedLatency = outDevInfo->defaultHighOutputLatency;
            
            err = Pa_OpenStream(
                &iStream,
                &inParams,
                &outParams,
                host_sample_rate,
                1024,
                paNoFlag,
                pa_callback,
                d
            );
            
            if (err != paNoError) {
                PORT_LOG("pa_openwasapi: All WASAPI attempts failed: %s (0x%x) - falling back to default PortAudio", 
                        Pa_GetErrorText(err), err);
                
                // Release MMCSS priority if set
                if (mmcssHandle) {
                    AvRevertMmThreadCharacteristics(mmcssHandle);
                }
                
                // Fall back to regular PortAudio
                return pa_open(0, 1, 1);
            }
            
            PORT_LOG("pa_openwasapi: Successfully opened WASAPI with high latency settings");
        } else {
            PORT_LOG("pa_openwasapi: Successfully opened WASAPI with recommended latency");
        }
    } else {
        PORT_LOG("pa_openwasapi: Successfully opened WASAPI in conservative shared mode");
    }

    // 10) Set up for single stream operation
    oneStream = 1;
    oStream = iStream;
    
    // 11) Log final stream configuration
    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(iStream);
    if (streamInfo) {
        PORT_LOG("pa_openwasapi: Stream configured with input latency=%.1fms, output latency=%.1fms, sample rate=%.1fHz",
                streamInfo->inputLatency * 1000.0,
                streamInfo->outputLatency * 1000.0,
                streamInfo->sampleRate);
    }
    
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

// Implementation of handle_paerror function
static void handle_paerror(PaError err, char * where)
{
    if (err != paNoError) {
        PORT_LOG("%s: PortAudio error: %s", where, Pa_GetErrorText(err));
        error_count++;
        
        if (error_count > 20) {
            PORT_LOG("%s: Too many PortAudio errors (%d), audio quality may be degraded", 
                    where, error_count);
            
            // Log a user-visible message for persistent audio issues
            if (error_count % 10 == 0) { // Only show every 10th error
                iaxci_usermsg(IAXC_TEXT_TYPE_NOTICE, 
                    "Audio system experiencing issues. You may need to restart the application if audio quality degrades.");
            }
        }
    }
}

static int pa_start(struct iaxc_audio_driver *d)
{
	static int errcnt = 0;
	current_audio_format = paInt16;  // Fix for 0x0 format issue

#ifdef _WIN32
	// Initialize critical section if not already done
	if (!pa_stream_lock_initialized) {
		InitializeCriticalSection(&pa_stream_lock);
		pa_stream_lock_initialized = 1;
		PORT_LOG("pa_start: Initialized critical section for thread safety");
	}
#endif

	if (running)
		return 0;

	PORT_LOG("iaxclient PortAudio module built on %s at %s", __DATE__, __TIME__);
	PORT_LOG("pa_start: Setting up audio with format 0x%x (paInt16)", current_audio_format);

	// Add format check and fix
	if (d != NULL) {
		PORT_LOG("pa_start: Audio format before start: 0x%x %s", 
			current_audio_format, 
			(current_audio_format == 0) ? "INVALID!" : "ok");
		
		// Fix zero format if needed
		if (current_audio_format == 0) {
			current_audio_format = paInt16;
			PORT_LOG("pa_start: Fixed zero format to 0x%x (paInt16)", current_audio_format);
		}
	}
	
	// Close mixers if already opened
	if (iMixer) {
		Px_CloseMixer(iMixer);
		iMixer = NULL;
	}

	if (oMixer) {
		Px_CloseMixer(oMixer);
		oMixer = NULL;
	}

	// Check for too many errors
	if (errcnt > 5) {
		iaxci_usermsg(IAXC_TEXT_TYPE_FATALERROR,
			"iaxclient audio: Can't open Audio Device. "
			"Perhaps you do not have an input or output device?");
		PORT_LOG("pa_start: Unable to open audio device after 5 attempts. Giving up.");
		iaxc_millisleep(1000);
		// Still try one more time instead of giving up
	}

	// Flush and reinitialize the ring buffers
	PaUtil_InitializeRingBuffer(&inRing, sizeof(SAMPLE), INRBSZ, inRingBuf);
	PaUtil_InitializeRingBuffer(&outRing, sizeof(SAMPLE), OUTRBSZ, outRingBuf);

	// Try to open audio streams
	if (pa_openstreams(d)) {
		errcnt++;
		PORT_LOG("pa_start: Failed to open audio streams, error count now %d", errcnt);
		return -1;
	}

	// Reset error counter on success
	errcnt = 0;

#ifdef _WIN32
	// On Windows, set higher thread priority for the audio thread process
	// This helps reduce audio glitches during system load
	if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
		PORT_LOG("pa_start: Failed to set process priority: %d", GetLastError());
	} else {
		PORT_LOG("pa_start: Set process to HIGH_PRIORITY_CLASS");
	}
	
	// Set the calling thread to time-critical priority
	if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
		PORT_LOG("pa_start: Failed to set thread priority: %d", GetLastError());
	} else {
		PORT_LOG("pa_start: Set main thread to THREAD_PRIORITY_TIME_CRITICAL");
	}
	
	// Configure Windows audio session for optimal quality
	pa_setup_windows_audio_session();
#endif

	// Start input stream
	PaError err = Pa_StartStream(iStream);
	if (err != paNoError) {
		PORT_LOG("pa_start: Failed to start input stream: %s", Pa_GetErrorText(err));
		return -1;
	}

	// Open mixer for input
	iMixer = Px_OpenMixer(iStream, 0);

	// Handle separate output stream if needed
	if (!oneStream) {
		PaError err = Pa_StartStream(oStream);
		oMixer = Px_OpenMixer(oStream, 0);
		if (err != paNoError) {
			PORT_LOG("pa_start: Failed to start output stream: %s", Pa_GetErrorText(err));
			Pa_StopStream(iStream);
			return -1;
		}
	}

	// Handle ring stream for auxiliary sounds if needed
	if (selectedRing != selectedOutput) {
		auxStream = 1;
	} else {
		auxStream = 0;
	}
	if (auxStream) {
		pa_openauxstream(d);
		if (Pa_StartStream(aStream) != paNoError) {
			PORT_LOG("pa_start: Failed to start auxiliary stream");
			auxStream = 0;
		} else {
			PORT_LOG("pa_start: Started auxiliary stream for ring sounds");
		}
	}

	// Configure audio input settings if mixer is available
	if (iMixer != NULL && !mixers_initialized) {
		// Try to select the microphone input source
		if (Px_SetCurrentInputSourceByName(iMixer, "microphone") != 0) {
			int n = Px_GetNumInputSources(iMixer) - 1;
			for (; n > 0; --n) {
				if (!strcasecmp("microphone", Px_GetInputSourceName(iMixer, n))) {
					Px_SetCurrentInputSource(iMixer, n);
					PORT_LOG("pa_start: Using microphone input source %d", n);
				}
			}
		}

		// Disable microphone boost to prevent clipping
		// Low levels can be fixed by software, but clipping cannot
		Px_SetMicrophoneBoost(iMixer, 0);
		PORT_LOG("pa_start: Disabled microphone boost to prevent clipping");

		// If input level is very low, raise it to ensure AGC can detect speech
		if (pa_input_level_get(d) < 0.5f) {
			pa_input_level_set(d, 0.6f);
			PORT_LOG("pa_start: Increased input level to 0.6 for AGC");
		}
				mixers_initialized = 1;
	}

#ifdef _WIN32
	// Health check is now managed by the timer-based system in pa_initialize
	// No need to create additional threads here to avoid duplication
#endif

	PORT_LOG("pa_start: Audio streams started successfully");
	running = 1;
	error_count = 0;
	output_underruns = 0;
	return 0;
}

static int pa_stop (struct iaxc_audio_driver *d)
{
	PaError err;

#ifdef _WIN32
	// Initialize critical section if not already done
	if (!pa_stream_lock_initialized) {
		InitializeCriticalSection(&pa_stream_lock);
		pa_stream_lock_initialized = 1;
	}
	
	// Enter critical section to prevent concurrent access
	EnterCriticalSection(&pa_stream_lock);
#endif

	if (!running) {
#ifdef _WIN32
		LeaveCriticalSection(&pa_stream_lock);
#endif
		return 0;
	}

	// Keep the audio system running if sounds are being played
	if (sounds) {
#ifdef _WIN32
		LeaveCriticalSection(&pa_stream_lock);
#endif
		return 0;
	}

	PORT_LOG("pa_stop: Stopping PortAudio streams (with thread protection)");

	// Attempt error recovery if streams are in a bad state
#ifdef _WIN32
	// First check if iStream is valid before using it
	if (iStream != NULL) {
		PaError inputStatus = Pa_IsStreamActive(iStream);
		if (inputStatus == 1) {
			// Normal active stream - proceed with abort
			err = Pa_AbortStream(iStream);
			if (err != paNoError) {
				PORT_LOG("pa_stop: Error aborting input stream: %s", Pa_GetErrorText(err));
			}
		} else if (inputStatus < 0) {
			// Stream in error state - try to recover
			PORT_LOG("pa_stop: Input stream in error state (%d), attempting recovery", inputStatus);
			Pa_CloseStream(iStream);
			iStream = NULL;
		} else {
			// Stream already stopped
			err = Pa_CloseStream(iStream);
		}
	} else {
		PORT_LOG("pa_stop: Input stream is NULL, skipping cleanup");
	}
#else	// For non-Windows, use the standard approach
	if (iStream != NULL) {
		err = Pa_AbortStream(iStream);
		err = Pa_CloseStream(iStream);
		iStream = NULL;
	} else {
		PORT_LOG("pa_stop: Input stream is NULL, skipping cleanup (non-Windows)");
	}
#endif

	if (!oneStream) {
		// Handle output stream separately if using separate streams
#ifdef _WIN32
		if (oStream != NULL) {
			PaError outputStatus = Pa_IsStreamActive(oStream);
			if (outputStatus == 1) {
				err = Pa_AbortStream(oStream);
				if (err != paNoError) {
					PORT_LOG("pa_stop: Error aborting output stream: %s", Pa_GetErrorText(err));
				}
			} else if (outputStatus < 0) {
				PORT_LOG("pa_stop: Output stream in error state (%d), attempting recovery", outputStatus);
				Pa_CloseStream(oStream);
				oStream = NULL;
			} else {
				err = Pa_CloseStream(oStream);
			}
		} else {
			PORT_LOG("pa_stop: Output stream is NULL, skipping cleanup");
		}
#else
		if (oStream != NULL) {
			err = Pa_AbortStream(oStream);
			err = Pa_CloseStream(oStream);
		}
#endif
	}

	if (auxStream && aStream != NULL) {
#ifdef _WIN32
		PaError auxStatus = Pa_IsStreamActive(aStream);
		if (auxStatus == 1) {
			err = Pa_AbortStream(aStream);
			if (err != paNoError) {
				PORT_LOG("pa_stop: Error aborting auxiliary stream: %s", Pa_GetErrorText(err));
			}
		} else if (auxStatus < 0) {
			PORT_LOG("pa_stop: Auxiliary stream in error state (%d), attempting recovery", auxStatus);
			Pa_CloseStream(aStream);
			aStream = NULL;
		} else {
			err = Pa_CloseStream(aStream);
		}
#else
		err = Pa_AbortStream(aStream);
		err = Pa_CloseStream(aStream);
#endif
	} else if (auxStream) {
		PORT_LOG("pa_stop: Auxiliary stream variable is true but aStream pointer is NULL");
	}

	// Clean up resamplers to avoid memory leaks
	if (speex_resampler) {
		speex_resampler_destroy(speex_resampler);
		speex_resampler = NULL;
	}
	
	if (output_resampler) {
		speex_resampler_destroy(output_resampler);
		output_resampler = NULL;
	}

#ifdef _WIN32
	// Stop health check thread using atomic operations
	if (InterlockedCompareExchange(&healthCheckThreadActive, 0, 1) == 1) {
		// Thread was active, wait for it to finish with timeout
		if (healthCheckThread) {
			WaitForSingleObject(healthCheckThread, 1000);
			CloseHandle(healthCheckThread);
			healthCheckThread = NULL;
			PORT_LOG("pa_stop: Health check thread terminated");
		}
	}
#endif

	PORT_LOG("pa_stop: Audio streams stopped (with thread protection)");
	running = 0;

#ifdef _WIN32
	// Leave critical section
	LeaveCriticalSection(&pa_stream_lock);
#endif

	return 0;
}

// Modify the pa_check_stream_health function to use our buffer booster
static int pa_check_stream_health(struct iaxc_audio_driver *d)
{    // This function is called periodically to check the health of audio streams
    
    // Much more conservative error threshold to avoid false positives
    if (error_count > 50) {  // Increased from 15 to 50
#ifdef VERBOSE        
        PORT_LOG("pa_check_stream_health: Too many errors (%d), requesting restart", error_count);
#endif
        return 1; // Need restart
    }
    
    // Much more tolerant underrun thresholds - PTT operation can cause temporary underruns
    static int startup_grace_period = 200; // Longer grace period
    int underrun_threshold = (startup_grace_period > 0) ? 300 : 200; // Much higher thresholds
    
    if (startup_grace_period > 0) {
        startup_grace_period--;
    }
    
    if (output_underruns > underrun_threshold) {
#ifdef VERBOSE        
        PORT_LOG("pa_check_stream_health: Too many underruns (%d > %d), requesting restart", 
                output_underruns, underrun_threshold);
#endif
        return 1; // Need restart
    }      // Be much more conservative about buffer boosting - only when really needed
    if (output_underruns > 50) {  // Much higher threshold to avoid interference with PTT
        pa_boost_buffer();
    }
    
    // Check stream states for errors
    if (iStream) {
        PaError status = Pa_IsStreamActive(iStream);
        if (status < 0) {
            PORT_LOG("pa_check_stream_health: Input stream has error state (%d)", status);
            return 1; // Need restart
        }
        
        // Check for latency or xruns when we can access this info
        const PaStreamInfo* info = Pa_GetStreamInfo(iStream);
        if (info) {
            if (info->inputLatency > 0.5 || info->outputLatency > 0.5) {
                PORT_LOG("pa_check_stream_health: Stream latency too high (in=%.1fms, out=%.1fms)",
                        info->inputLatency * 1000, info->outputLatency * 1000);
                return 1; // Need restart with better parameters
            }
        }
    }
    
    if (oStream && oStream != iStream) {
        PaError status = Pa_IsStreamActive(oStream);
        if (status < 0) {
            PORT_LOG("pa_check_stream_health: Output stream has error state (%d)", status);
            return 1; // Need restart
        }
    }
    
    // Check if ring buffer is severely under/over filled
    int inRingBufferFill = PaUtil_GetRingBufferFullCount(&inRing);
    int outRingBufferFill = PaUtil_GetRingBufferFullCount(&outRing);
    
    // Extreme conditions that need recovery
    if (inRingBufferFill > INRBSZ * 0.9) {
        PORT_LOG("pa_check_stream_health: Input ring buffer overflow (%.1f%% full)", 
                (float)inRingBufferFill / INRBSZ * 100);
        // Purge half the buffer
        SAMPLE dummy[1024];
        int toPurge = inRingBufferFill / 2;
        while (toPurge > 0) {
            int count = toPurge > 1024 ? 1024 : toPurge;
            PaUtil_ReadRingBuffer(&inRing, dummy, count);
            toPurge -= count;
        }
        PORT_LOG("pa_check_stream_health: Purged input buffer to %.1f%% full", 
                (float)PaUtil_GetRingBufferFullCount(&inRing) / INRBSZ * 100);
        return 0; // Fixed with purge
    }
      if (outRingBufferFill < 80 && output_underruns > 5) {  // Optimized for low latency
        PORT_LOG("pa_check_stream_health: Output ring buffer critically low (%d samples, %d underruns)", 
                outRingBufferFill, output_underruns);
        
        // Use our low-latency buffer boosting function
        pa_boost_buffer();
        return 0; // Fixed with minimal silence buffer
    }
    
    // All checks passed
    return 0; // Healthy
}

static int pa_input(struct iaxc_audio_driver *d, void *samples, int *nSamples)
{
    static int error_count = 0;
    static int last_success_time = 0;
    static int call_count = 0;
    static int total_samples_read = 0;
    static int last_stats_time = 0;
    static int last_health_check = 0;
    int current_time = time(NULL);
    int elementsToRead = *nSamples;
    int available = PaUtil_GetRingBufferReadAvailable(&inRing);
    
    // Periodically check audio stream health (every 5 seconds)
    if (current_time - last_health_check >= 5) {
        if (pa_check_stream_health(d)) {
#ifdef VERBOSE            
            PORT_LOG("pa_input: Stream health check detected and recovered from a problem");
#endif
            // If we had a problem, reset the error count and last success time            
            // After recovery, reset some stats
            error_count = 0;
            last_success_time = current_time;
        }
        last_health_check = current_time;
    }
    
    // Collect stats about how much data we're reading
    call_count++;
    
    // Log stats periodically to help with debugging
    if (current_time - last_stats_time >= 30) {  // Every 30 seconds
#ifdef VERBOSE        
        PORT_LOG("pa_input: STATS: %d calls in %d seconds, %d total samples read (%.1f samples/call)",
                call_count, current_time - last_stats_time, 
                total_samples_read, 
                (float)total_samples_read / (call_count > 0 ? call_count : 1));
#endif
        call_count = 0;
        total_samples_read = 0;
        last_stats_time = current_time;
    }
    
    // Check if we have enough data in the ring buffer
    if (available < elementsToRead) {
        // Return partial data if we have more than half of what was requested
        if (available > elementsToRead/2) {
            PaUtil_ReadRingBuffer(&inRing, samples, available);
            *nSamples = available;
            total_samples_read += available;
            error_count = 0;
            last_success_time = current_time;
            return 0;  // Success with partial data
        }
        
        // During startup phase or after silence, return 0 samples without error
        if (startup_counter++ < 200) {
            *nSamples = 0;
            return 0;  // Return success with 0 samples during startup
        }
        
        // Add small delay and retry logic for transient buffer underruns
        if (++error_count > 5) {
            // If we've had many errors in a row, log it and sleep briefly
            PORT_LOG("pa_input: Multiple buffer underruns (%d in a row). Available=%d, needed=%d", 
                    error_count, available, elementsToRead);
            
            // Before giving up, check how long since our last success
            if (current_time - last_success_time > 10) {
                // It's been a while since we had success - could be a serious issue
                PORT_LOG("pa_input: No successful reads for %d seconds - audio input may be stalled",
                        current_time - last_success_time);
            }
            
            error_count = 0;
            // Small sleep to allow buffer to fill
            iaxc_millisleep(5);
        }
        
        *nSamples = 0;
        return 1;  // Error - not enough data
    }
    
    // Success - we have enough data
    PaUtil_ReadRingBuffer(&inRing, samples, elementsToRead);
    total_samples_read += elementsToRead;
    error_count = 0;
    startup_counter = 0;  // Reset startup counter on success
    last_success_time = current_time;
    
    // Check for silent input (could indicate a problem with the mic)
    if (call_count % 100 == 0) {  // Only check occasionally
        int is_silent = 1;
        short* sample_data = (short*)samples;
        int silent_threshold = 5;  // Extremely low level to detect true silence
        
        // Check first 100 samples (or fewer if we have less)
        for (int i = 0; i < elementsToRead && i < 100; i++) {
            if (abs(sample_data[i]) > silent_threshold) {
                is_silent = 0;
                break;
            }
        }
        
        // Only log if we have many samples to check and they're all silent
        if (is_silent && elementsToRead >= 100) {
            // Keep track of consecutive silent reads
            static int consecutive_silent = 0;
            consecutive_silent++;
            
            if (consecutive_silent % 50 == 0) {  // Log after ~5 seconds of silence
#ifdef VERBOSE
                PORT_LOG("pa_input: WARNING: Detected %d consecutive silent inputs - check microphone",
                        consecutive_silent);
#endif
            }
        } else {
            // Reset the counter when we get non-silent audio
            static int consecutive_silent = 0;
            consecutive_silent = 0;
        }
    }
    
    return 0;  // Success
}

static int pa_output(struct iaxc_audio_driver *d, void *samples, int nSamples)
{
    // Get ring buffer metrics
    int outRingLen = PaUtil_GetRingBufferWriteAvailable(&outRing);
    outRingLenAvg = (outRingLenAvg * 9 + outRingLen) / 10;
    
    // Periodically check stream health
    static int last_health_check = 0;
    int current_time = time(NULL);
    if (current_time - last_health_check >= 5) {
        // Only check if we've been experiencing problems
        static int consecutive_errors = 0;
        if (consecutive_errors > 3) {
            if (pa_check_stream_health(d)) {
                PORT_LOG("pa_output: Stream health check recovered from a problem");
                consecutive_errors = 0;
            }
        }
        last_health_check = current_time;
    }
    
    // Safety check for invalid samples
    if (!samples || nSamples <= 0) {
        PORT_LOG("pa_output: Invalid samples: ptr=%p, count=%d", samples, nSamples);
        return 0;
    }
    
    // Check for buffer overflow
    if (outRingLen < nSamples) {
        // This is a buffer overflow situation - we need to handle it gracefully
        
        // Strategy 1: If slightly over, drop a fraction of samples to catch up
        if (outRingLen > nSamples * 0.75) {
            // Write what we can, skipping some samples
            int skip_ratio = nSamples / outRingLen + 1;
            SAMPLE* in_samples = (SAMPLE*)samples;
            SAMPLE temp_buf[1024]; // Use a small temp buffer
            
            // Downsample by skipping some samples
            int out_count = 0;
            for (int i = 0; i < nSamples && out_count < outRingLen && out_count < 1024; i++) {
                if (i % skip_ratio != 0) {  // Skip some samples
                    temp_buf[out_count++] = in_samples[i];
                }
            }
            
            // Write the downsampled buffer
            int written = PaUtil_WriteRingBuffer(&outRing, temp_buf, out_count);
            PORT_LOG("pa_output: Partial overflow, downsampled %d samples to %d", 
                     nSamples, written);
            return written;
        }
        
        // Strategy 2: For severe overflow, drop all samples and log
        PORT_LOG("pa_output: Buffer overflow - dropping %d samples (only %d available)", 
                nSamples, outRingLen);
        
        // Count the total number of dropped samples for diagnostics
        static int total_dropped = 0;
        total_dropped += nSamples;
        
        if (total_dropped % 8000 == 0) {  // Log roughly every second of dropped audio
            PORT_LOG("pa_output: Dropped %d total samples (%d seconds of audio)",
                    total_dropped, total_dropped/8000);
        }
        
        return 0;
    }
    
    // No overflow condition - write the data to the ring buffer
    int written = PaUtil_WriteRingBuffer(&outRing, samples, nSamples);
      // Track metrics for buffer fullness
    static int last_report_time = 0;
    static int total_samples = 0;
    
    total_samples += written;
    
    // Report buffer metrics periodically
    if (current_time - last_report_time >= 10) {  // Every 10 seconds
        float buffer_fullness = (float)(OUTRBSZ - outRingLen) / OUTRBSZ * 100.0f;
#ifdef VERBOSE        
        PORT_LOG("pa_output: Buffer stats - %d%% full, %d samples/sec avg",
                (int)buffer_fullness, total_samples / (current_time - last_report_time));
#endif        
        last_report_time = current_time;
        total_samples = 0;
    }
    
    
    // Log any partial writes (but this should never happen with the overflow check above)
    if (written < nSamples) {
        PORT_LOG("pa_output: Unexpectedly wrote only %d of %d samples", written, nSamples);
    }
    
    return written;
}

// Low-latency adaptive buffer stabilizer function
static void pa_boost_buffer(void)
{
    int available = PaUtil_GetRingBufferReadAvailable(&outRing);
    int capacity = OUTRBSZ;
    float fullness = (float)available / capacity;
    
    // Only boost if buffer is critically low (< 10%)
    if (fullness < 0.1) {
        // Target a much smaller buffer - just enough to prevent immediate underrun
        // This targets ~100ms worth of audio at 8kHz instead of 800ms
        int target_samples = sample_rate / 10;  // 100ms worth of samples
        int to_add = target_samples - available;
        
        if (to_add > 0) {
            // Limit boost to prevent excessive latency
            if (to_add > 2048) to_add = 2048;  // Max ~256ms boost at 8kHz
            SAMPLE silence[1024] = {0};
            
            int added = 0;
            while (added < to_add) {
                int chunk = (to_add - added > 1024) ? 1024 : (to_add - added);
                added += PaUtil_WriteRingBuffer(&outRing, silence, chunk);
            }
            
            PORT_LOG("pa_boost_buffer: Added %d silence samples (%.1f%% -> %.1f%%) for low-latency stability",
                    added, fullness * 100, (float)(available + added) / capacity * 100);
            output_underruns = 0;
        }
    }
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
		
		// For some errors, try one more time after a delay
		if (err == paUnanticipatedHostError) {
		    PORT_LOG("_pa_initialize:Unanticipated host error, trying again after delay");
		    iaxc_millisleep(500);
		    
		    if (paNoError != (err = Pa_Initialize())) {
		        PORT_LOG("_pa_initialize:Second attempt also failed: %s", Pa_GetErrorText(err));
		        iaxci_usermsg(IAXC_TEXT_TYPE_ERROR, "Failed to initialize audio system");
		        return err;
		    }
		    
		    PORT_LOG("_pa_initialize:Second attempt succeeded");
		} else {
		    iaxci_usermsg(IAXC_TEXT_TYPE_ERROR, "Failed to initialize audio system");
		    return err;
		}
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

	// Configure for Windows optimization
#ifdef _WIN32
	// Pre-initialize host sample rate to a reasonable default
	// Will be updated when devices are actually opened
	host_sample_rate = 48000.0;
	sample_ratio = host_sample_rate / (double)sample_rate;
	
	PORT_LOG("_pa_initialize: Windows-specific optimizations enabled");
#endif

	// Initialize ring buffers for audio processing
	PaUtil_InitializeRingBuffer(&inRing, sizeof(SAMPLE), INRBSZ, inRingBuf);
	PaUtil_InitializeRingBuffer(&outRing, sizeof(SAMPLE), OUTRBSZ, outRingBuf);
	// Explicitly flush ring buffers to ensure they're clean
	PaUtil_FlushRingBuffer(&inRing);
	PaUtil_FlushRingBuffer(&outRing);

	// Add some silence at the beginning to prime the output buffer
	// This helps prevent initial audio underruns
	SAMPLE silence[512] = {0};  // Larger silence buffer for stability
	PaUtil_WriteRingBuffer(&outRing, silence, 512);
	
	PORT_LOG("_pa_initialize: Ring buffers initialized with %d bytes input and %d bytes output",
		INRBSZ * sizeof(SAMPLE), OUTRBSZ * sizeof(SAMPLE));
	
	// Initialize memory for error recovery
	error_count = 0;
	startup_counter = 0;
	output_underruns = 0;	running = 0;
	
	PORT_LOG("_pa_initialize: PortAudio initialization complete");
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

    // Set up health check timer to periodically refresh audio (helps with long-term stability)
#ifdef _WIN32
    static HANDLE healthCheckTimer = NULL;
    static BOOL timerInitialized = FALSE;
      if (!timerInitialized) {
        // Create a timer for periodic health checks - less frequent to avoid interfering with normal operation
        HANDLE hTimer = CreateWaitableTimer(NULL, TRUE, "IAXAudioHealthTimer");
        if (hTimer) {
            LARGE_INTEGER liDueTime;
            // Start with 10 minutes instead of 1 hour for initial check, then extend if healthy
            liDueTime.QuadPart = -6000000000LL; // 10 minutes in 100ns units
            
            if (SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE)) {
                // Create a thread to wait for the timer
                HANDLE hThread = CreateThread(NULL, 0, 
                    (LPTHREAD_START_ROUTINE)HealthCheckTimerThread, 
                    d, 0, NULL);
                
                if (hThread) {
                    PORT_LOG("pa_initialize: Scheduled health check every 10 minutes for stability monitoring");
                    CloseHandle(hThread);
                    timerInitialized = TRUE;
                    healthCheckTimer = hTimer;
                } else {
                    PORT_LOG("pa_initialize: Failed to create health check thread");
                    CloseHandle(hTimer);
                }
            } else {
                PORT_LOG("pa_initialize: Failed to set waitable timer");
                CloseHandle(hTimer);
            }
        }
    }
#endif

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

#ifdef _WIN32
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

static void pa_setup_windows_audio_session(void)
{
    HRESULT hr;
    IMMDeviceEnumerator *pEnumerator = NULL;
    IMMDevice *pDevice = NULL;
    IAudioSessionManager *pSessionManager = NULL;
    IAudioSessionControl *pSessionControl = NULL;
    IAudioSessionControl2 *pSessionControl2 = NULL;
    
    // Initialize COM
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: COM initialized");
    } else {
        PORT_LOG("pa_setup_windows_audio_session: Failed to initialize COM: 0x%x", hr);
        return;
    }
    
    // Get the device enumerator - ensure we have the correct GUID
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, 
                          &IID_IMMDeviceEnumerator, (void**)&pEnumerator);
    if (FAILED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: Failed to create device enumerator: 0x%x", hr);
        CoUninitialize();
        return;
    }
    
    // Get the default audio endpoint
    hr = pEnumerator->lpVtbl->GetDefaultAudioEndpoint(pEnumerator, eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: Failed to get default endpoint: 0x%x", hr);
        pEnumerator->lpVtbl->Release(pEnumerator);
        CoUninitialize();
        return;
    }
    
    // Get the session manager
    hr = pDevice->lpVtbl->Activate(pDevice, &IID_IAudioSessionManager, 
                                  CLSCTX_ALL, NULL, (void**)&pSessionManager);
    if (FAILED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: Failed to get session manager: 0x%x", hr);
        pDevice->lpVtbl->Release(pDevice);
        pEnumerator->lpVtbl->Release(pEnumerator);
        CoUninitialize();
        return;
    }
    
    // Get the audio session control
    hr = pSessionManager->lpVtbl->GetAudioSessionControl(pSessionManager, NULL, 0, &pSessionControl);
    if (FAILED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: Failed to get session control: 0x%x", hr);
        pSessionManager->lpVtbl->Release(pSessionManager);
        pDevice->lpVtbl->Release(pDevice);
        pEnumerator->lpVtbl->Release(pEnumerator);
        CoUninitialize();
        return;
    }
    
    // Get the extended session control interface
    hr = pSessionControl->lpVtbl->QueryInterface(pSessionControl, 
                                              &IID_IAudioSessionControl2, (void**)&pSessionControl2);
    if (FAILED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: Failed to get session control2: 0x%x", hr);
        pSessionControl->lpVtbl->Release(pSessionControl);
        pSessionManager->lpVtbl->Release(pSessionManager);
        pDevice->lpVtbl->Release(pDevice);
        pEnumerator->lpVtbl->Release(pEnumerator);
        CoUninitialize();
        return;
    }
    
    // Set the session to not be ducked by Windows (e.g. during notifications)
    hr = pSessionControl2->lpVtbl->SetDuckingPreference(pSessionControl2, TRUE);
    if (SUCCEEDED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: Successfully set ducking preference");
    }
    
    // Set display name for the audio session
    hr = pSessionControl->lpVtbl->SetDisplayName(pSessionControl, L"IAX Audio", NULL);
    if (SUCCEEDED(hr)) {
        PORT_LOG("pa_setup_windows_audio_session: Set session display name to 'IAX Audio'");
    }
    
    // Clean up
    pSessionControl2->lpVtbl->Release(pSessionControl2);
    pSessionControl->lpVtbl->Release(pSessionControl);
    pSessionManager->lpVtbl->Release(pSessionManager);
    pDevice->lpVtbl->Release(pDevice);
    pEnumerator->lpVtbl->Release(pEnumerator);
    
    CoUninitialize();
      PORT_LOG("pa_setup_windows_audio_session: Audio session configured for improved priority");
}
#endif

#ifdef _WIN32
// Health check timer thread function
static DWORD WINAPI HealthCheckTimerThread(LPVOID param)
{
    struct iaxc_audio_driver *d = (struct iaxc_audio_driver *)param;    HANDLE hTimer = CreateWaitableTimer(NULL, TRUE, "IAXAudioHealthTimer");
    if (hTimer) {
        LARGE_INTEGER liDueTime;
        // Initial check after 10 minutes - less frequent to avoid interfering with normal operation
        liDueTime.QuadPart = -6000000000LL; // 10 minutes in 100ns units
        
        if (SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE)) {
            InterlockedExchange(&healthCheckThreadActive, 1);
            
            PORT_LOG("HealthCheckTimerThread: Started with 10-minute initial interval for stability monitoring");
            
            // Track statistics for health monitoring
            int restart_count = 0;
            int last_restart_time = GetTickCount();
            
            while (InterlockedRead(&healthCheckThreadActive)) {
                // Wait for the timer to trigger - longer timeout to reduce CPU usage
                DWORD result = WaitForSingleObject(hTimer, 30000); // Check every 30 seconds for thread exit
                
                if (!InterlockedRead(&healthCheckThreadActive)) {
                    break; // Exit thread if requested
                }
                
                if (result == WAIT_OBJECT_0) {
                    PORT_LOG("HealthCheckTimerThread: Performing periodic health check");
                    
                    // Check if currently running before performing health maintenance
                    // Use critical section to safely check stream state
                    if (pa_stream_lock_initialized) {
                        EnterCriticalSection(&pa_stream_lock);
                        int is_running = running;
                        LeaveCriticalSection(&pa_stream_lock);
                        
                        if (is_running) {
                            // Do a thorough health check
                            if (pa_check_stream_health(d)) {
                                // Stream health check indicates problems that need a restart
                                PORT_LOG("HealthCheckTimerThread: Health check detected issues, restarting audio (protected)");
                                
                                // Check if we're restarting too frequently (possible device issues)
                                DWORD current_time = GetTickCount();                                if (current_time - last_restart_time < 300000 && restart_count > 1) { // 5 minutes instead of 1 minute
                                    // Too many restarts in a short time, increase interval to prevent thrashing
                                    PORT_LOG("HealthCheckTimerThread: Too many restarts (%d) in short period, extending interval", 
                                            restart_count);
                                    
                                    // Show a warning to the user
                                    iaxci_usermsg(IAXC_TEXT_TYPE_NOTICE, 
                                        "Audio system experiencing issues. Check your audio device settings.");
                                    
                                    // Set next check farther away (2 hours)
                                    liDueTime.QuadPart = -72000000000LL; // 2 hours
                                    restart_count = 0;
                                } else {
                                    // Set next check in 30 minutes instead of 5
                                    liDueTime.QuadPart = -18000000000LL; // 30 minutes
                                    restart_count++;
                                }
                                
                                // Perform the actual restart with proper synchronization
                                // The pa_stop and pa_start functions now have their own critical sections
                                pa_stop(d);
                                iaxc_millisleep(500); // Give time for resources to be released
                                pa_start(d);
                                
                                last_restart_time = GetTickCount();
                                PORT_LOG("HealthCheckTimerThread: Audio streams restarted successfully");
                            } else {
                                // No problems detected, perform routine maintenance
                                PORT_LOG("HealthCheckTimerThread: System healthy, extending check interval");
                                
                                // Reset counters for good behavior
                                error_count = 0;
                                
                                // Set next check in 1 hour for healthy systems
                                liDueTime.QuadPart = -36000000000LL; // 1 hour
                                restart_count = 0;
                            }
                        } else {
                            // Audio not running, just reset the timer
                            PORT_LOG("HealthCheckTimerThread: Audio not running, skipping health check");
                            liDueTime.QuadPart = -18000000000LL; // 30 minutes
                        }                        } else {
                            // Critical section not initialized, skip this check
                            PORT_LOG("HealthCheckTimerThread: Critical section not initialized, skipping health check");
                            liDueTime.QuadPart = -18000000000LL; // 30 minutes
                        }
                    
                    // Set the timer for the next check
                    SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, FALSE);
                }
            }
        }
        CloseHandle(hTimer);
    }
    
    PORT_LOG("HealthCheckTimerThread: Thread terminated (protected)");
    return 0;
}
#endif