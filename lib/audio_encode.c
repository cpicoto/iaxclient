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
 *
 * This program is free software, distributed under the terms of
 * the GNU Lesser (Library) General Public License.
 */

 #include "iaxclient.h"
 #include "audio_encode.h"

// External reference to global debug flag
extern int iaxc_debug_enabled;
 #include "iaxclient_lib.h"
 #include "libiax2/src/iax-client.h"
 #ifdef CODEC_GSM
 #include "codec_gsm.h"
 #endif
 #include "codec_ulaw.h"
 #include "codec_alaw.h"
 
 #include "codec_speex.h"
 #include <speex/speex_preprocess.h>
 
 #ifdef CODEC_ILBC
 #include "codec_ilbc.h"
 #endif
 
 // Add headers for file I/O and time handling
 #include <stdio.h>
 #include <time.h>
 #include <string.h>
 #include <stdlib.h>

 #ifdef _WIN32
 #include <windows.h>
 #define AUDIO_LOG(fmt, ...)                                                    \
   do {                                                                           \
     if (iaxc_debug_enabled) {                                                    \
       char _buf[512];                                                            \
       char _time_buf[32];                                                        \
       SYSTEMTIME _st;                                                            \
       GetLocalTime(&_st);                                                        \
       snprintf(_time_buf, sizeof(_time_buf), "%02d:%02d:%02d.%03d",              \
                _st.wHour, _st.wMinute, _st.wSecond, _st.wMilliseconds);          \
       _snprintf(_buf, sizeof(_buf), "%s:[audio-debug] " fmt "\n",               \
                _time_buf, ##__VA_ARGS__);                                        \
       OutputDebugStringA(_buf);                                                  \
     }                                                                            \
   } while(0)
#else
 #include <time.h>
 #include <sys/time.h>
 #define OPENAL_LOG(fmt, ...)                                                    \
   do {                                                                           \
     if (iaxc_debug_enabled) {                                                    \
       struct timeval tv;                                                         \
       struct tm* tm_info;                                                        \
       char _time_buf[32];                                                        \
       gettimeofday(&tv, NULL);                                                   \
       tm_info = localtime(&tv.tv_sec);                                           \
       strftime(_time_buf, sizeof(_time_buf), "%H:%M:%S", tm_info);               \
       char _ms_buf[8];                                                           \
       snprintf(_ms_buf, sizeof(_ms_buf), ".%03d", (int)(tv.tv_usec / 1000));     \
       strcat(_time_buf, _ms_buf);                                                \
       fprintf(stderr, "[audio-debug %s] " fmt "\n", _time_buf, ##__VA_ARGS__);  \
     }                                                                            \
   } while(0)
#endif

// WAV file format structures
typedef struct {
    char     chunk_id[4];
    uint32_t chunk_size;
    char     format[4];
} RiffHeader;

typedef struct {
    char     chunk_id[4];
    uint32_t chunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtHeader;

typedef struct {
    char     chunk_id[4];
    uint32_t chunk_size;
} DataHeader;

// Global file handle for audio capture
static FILE* audio_capture_file = NULL;
static int audio_samples_written = 0;
static int audio_capture_sample_rate = 8000; // Default, will be updated

// Add these global variables after the existing ones
static time_t audio_capture_start_time = 0;
static int audio_capture_frame_count = 0;
static int audio_max_sample = 0;
static int audio_min_sample = 0;

// Audio level normalization parameters
static float target_level = 0.7f;         // Target level as fraction of max (about -3dB)
static float level_smoothing = 0.95f;     // Smoothing factor for level detection
static float current_level_peak = 0.0f;   // Current detected peak level
static float gain_smoothing = 0.98f;      // Smoothing factor for gain changes
static float current_gain = 1.0f;         // Current normalization gain

// Forward declarations for audio normalization functions
static short soft_clip(float sample);
static void normalize_audio_buffer(short *buffer, int samples);

float iaxci_silence_threshold = AUDIO_ENCODE_SILENCE_DB;

static float input_level = 0.0f;
static float output_level = 0.0f;

static SpeexPreprocessState *st = NULL;
static int speex_state_size = 0;
static int speex_state_rate = 0;

int iaxci_filters = IAXC_FILTER_AGC|IAXC_FILTER_DENOISE|IAXC_FILTER_AAGC|IAXC_FILTER_CN;

/* use to measure time since last audio was processed */
static struct timeval timeLastInput ;
static struct timeval timeLastOutput ;

static struct iaxc_speex_settings speex_settings =
{
	1,    /* decode_enhance */
	-1,   /* float quality */
	-1,   /* bitrate */
	0,    /* vbr */
	0,    /* abr */
	3     /* complexity */
};
static SpeexPreprocessState* st_small = NULL;  // For ~85 sample buffers
static SpeexPreprocessState* st_large = NULL;  // For 160 sample buffers

/* Forward declarations for PTT filter functions */
EXPORT void iaxc_ptt_filters_disable(void);
EXPORT void iaxc_ptt_filters_restore(void);
static void set_speex_filters_for_state(SpeexPreprocessState* state);

static float vol_to_db(float vol)
{
	/* avoid calling log10() on zero which yields inf or
	 * negative numbers which yield nan */
	if ( vol <= 0.0f )
		return AUDIO_ENCODE_SILENCE_DB;
	else
		return log10f(vol) * 20.0f;
}

static int do_level_callback()
{
	static struct timeval last = {0,0};
	struct timeval now;
	float input_db;
	float output_db;

	now = iax_tvnow();

	if ( last.tv_sec != 0 && iaxci_usecdiff(&now, &last) < 100000 )
		return 0;

	last = now;

	/* if input has not been processed in the last second, set to silent */
	input_db = iaxci_usecdiff(&now, &timeLastInput) < 1000000 ?
			vol_to_db(input_level) : AUDIO_ENCODE_SILENCE_DB;

	/* if output has not been processed in the last second, set to silent */
	output_db = iaxci_usecdiff(&now, &timeLastOutput) < 1000000 ?
		vol_to_db(output_level) : AUDIO_ENCODE_SILENCE_DB;

	iaxci_do_levels_callback(input_db, output_db);

	return 0;
}

static void set_speex_filters()
{
	int i;
    if (st_small)
        set_speex_filters_for_state(st_small);
    if (st_large)
        set_speex_filters_for_state(st_large);
    return;
	if ( !st )
		return;

	i = 1; /* always make VAD decision */
	speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &i);
	i = (iaxci_filters & IAXC_FILTER_AGC) ? 1 : 0;
	speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC, &i);
	i = (iaxci_filters & IAXC_FILTER_DENOISE) ? 1 : 0;
	speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_DENOISE, &i);

	/*
	* We can tweak these parameters to play with VAD sensitivity.
	* For now, we use the default values since it seems they are a good starting point.
	* However, if need be, this is the code that needs to change
	*/
	i = 35;
	speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &i);
	i = 20;
	speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &i);
}

static void calculate_level(short *audio, int len, float *level)
{
	int big_sample = 0;
	int i;

	for ( i = 0; i < len; i++ )
	{
		const int sample = abs(audio[i]);
		big_sample = sample > big_sample ?
			sample : big_sample;
	}

	*level += ((float)big_sample / 32767.0f - *level) / 5.0f;
}

// Modified to accept a specific preprocessor state with adaptive settings
static void set_speex_filters_for_state(SpeexPreprocessState* state)
{
    if (!state) return;

    int i;
    float f;
    
    /* Basic filter settings - always enabled */
    i = 1; /* always make VAD decision */
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_VAD, &i);
    
    /* AGC (Automatic Gain Control) */
    i = (iaxci_filters & IAXC_FILTER_AGC) ? 1 : 0;
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC, &i);
    
    if (i) {
        /* AGC settings - adaptive for better voice quality */
        i = 12000;  /* Increased from 8000 - better target level for voice */
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &i);
        
        /* AGC maximum gain (some versions of Speex may not support this) */
        #ifdef SPEEX_PREPROCESS_SET_AGC_MAX_GAIN
        i = 25;  /* Maximum gain factor */
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &i);
        #endif
    }
    
    /* Noise suppression/denoise filter */
    i = (iaxci_filters & IAXC_FILTER_DENOISE) ? 1 : 0;
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DENOISE, &i);
    
    /* VAD (Voice Activity Detection) settings - improved for better speech detection */
    i = 30;  /* Lower from 35 - easier to trigger voice detection */
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_PROB_START, &i);
    
    i = 25;  /* Higher from 20 - hold voice detection longer to avoid cutting off phrases */
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &i);
    
    /* Enable dereverb for improved speech clarity in all cases */
    i = 1;
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DEREVERB, &i);
    
    /* Dereverb settings - enhanced for better echo removal */
    f = 0.5f;  /* Increased from 0.4f - more aggressive decay handling */
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DEREVERB_DECAY, &f);
    
    f = 0.35f;  /* Increased from 0.3f - slightly more reverb reduction */
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DEREVERB_LEVEL, &f);
    
    AUDIO_LOG("Speex preprocessor configured with optimized voice settings");
}

static int input_postprocess(void *audio, int len, int rate)
{
	static int frame_count = 0;
    frame_count++;
    // Choose appropriate preprocessor state based on buffer size
    SpeexPreprocessState** active_st;

    // Print first few samples for inspection
    short *samples = (short *)audio;
    int print_count = len > 8 ? 8 : len;



	static float lowest_volume = 1.0f;
	float volume;
	int silent = 0;
#ifdef SAVE_LOCAL_AUDIO
	// Update the sample rate for WAV file creation
	audio_capture_sample_rate = rate;
	
	// Save audio to WAV file if capture is active
	if (audio_capture_file) {
		 // Add debug to check if audio data has content
        short *samples = (short *)audio;
        int has_content = 0;
        int max_sample = 0;
        
        // Check first few samples to see if there's any audio
        for (int i = 0; i < 20 && i < len; i++) {
            if (abs(samples[i]) > 100) { // Threshold for "non-silence"
                has_content = 1;
                if (abs(samples[i]) > max_sample)
                    max_sample = abs(samples[i]);
            }
        }  
        // Add statistics gathering
        for (int i = 0; i < len; i++) {
            if (samples[i] > audio_max_sample) audio_max_sample = samples[i];
            if (samples[i] < audio_min_sample) audio_min_sample = samples[i];
        }
        
        // Write raw PCM data
        size_t written = fwrite(audio, sizeof(short), len, audio_capture_file);
        if (written != len) {
            AUDIO_LOG("input_postprocess:WARNING: Failed to write all audio data: %zu/%d written", written, len);
        }
        audio_samples_written += len;
        audio_capture_frame_count++;
        
        // Flush to ensure data is written to disk
        fflush(audio_capture_file);
        
	}
#endif
    // Use different preprocessor instances based on frame size category
    if (len < 100) {
        // Small frame (~85 samples)
        active_st = &st_small;
        if (!*active_st) {
            *active_st = speex_preprocess_state_init(len, rate);
            speex_state_size = len;  // Keep track for debugging only
            speex_state_rate = rate;
            set_speex_filters_for_state(*active_st);
            AUDIO_LOG("Created small-frame preprocessor state: len=%d, rate=%d", len, rate);
        }
    }
    else {
        // Standard frame (160 samples)
        active_st = &st_large;
        if (!*active_st) {
            *active_st = speex_preprocess_state_init(len, rate);
            speex_state_size = len;  // Keep track for debugging only
            speex_state_rate = rate;
            set_speex_filters_for_state(*active_st);
            AUDIO_LOG("Created large-frame preprocessor state: len=%d, rate=%d", len, rate);
        }
    }

	calculate_level((short *)audio, len, &input_level);
#ifdef VERBOSE
    AUDIO_LOG("input_post_process: Calculated input level %4.4f", input_level);
#endif
	/* only preprocess if we're interested in VAD, AGC, or DENOISE */
    if ((iaxci_filters & (IAXC_FILTER_DENOISE | IAXC_FILTER_AGC)) ||
        iaxci_silence_threshold > 0.0f) {
        
        silent = !speex_preprocess(*active_st, (spx_int16_t*)audio, NULL);
#ifdef VERBOSE
        //Confirmed to be working 
        AUDIO_LOG("input_post_process: Calling speex_preprocess got silent=(%d)", silent);
#endif
    }

	/* Analog AGC: Bring speex AGC gain out to mixer, with lots of hysteresis */
	/* use a higher continuation threshold for AAGC than for VAD itself */
	if ( !silent &&
	     iaxci_silence_threshold != 0.0f &&
	     (iaxci_filters & IAXC_FILTER_AGC) &&
	     (iaxci_filters & IAXC_FILTER_AAGC)
	   )
	{
		static int i = 0;

		i++;

		if ( (i & 0x3f) == 0 )
		{
			float loudness;
#ifdef SPEEX_PREPROCESS_GET_AGC_LOUDNESS
			speex_preprocess_ctl(*active_st, SPEEX_PREPROCESS_GET_AGC_LOUDNESS, &loudness);
#else
			loudness = (*active_st)->loudness2;
#endif
            AUDIO_LOG("input_post_process: loudness=(%4.4f)", loudness);
			if ( loudness > 8000.0f || loudness < 4000.0f )
			{
				const float level = iaxc_input_level_get();
                AUDIO_LOG("input_post_process: iaxc_input_level_get: %4.4f, loudness=(%4.4f)", level,loudness);
				if ( loudness > 16000.0f && level > 0.5f )
				{
					/* lower quickly if we're really too hot */
					iaxc_input_level_set(level - 0.2f);
				}
				else if ( loudness > 8000.0f && level >= 0.15f )
				{
					/* lower less quickly if we're a bit too hot */
					iaxc_input_level_set(level - 0.1f);
				}
				else if ( loudness < 4000.0f && level <= 0.9f )
				{
					/* raise slowly if we're cold */
					iaxc_input_level_set(level + 0.1f);
				}
			}
		}
	}

	/* This is ugly. Basically just don't get volume level if speex thought
	 * we were silent. Just set it to 0 in that case */
	if ( iaxci_silence_threshold > 0.0f && silent )
		input_level = 0.0f;

	do_level_callback();

	volume = vol_to_db(input_level);

	if ( volume < lowest_volume )
		lowest_volume = volume;

    if (iaxci_silence_threshold > 0.0f) {
#ifdef VERBOSE
        AUDIO_LOG("input_postprocess: iaxci_silence_threshold > 0.0f => declaring SILENCE (%d)", silent);
#endif
        return silent;
    }
    else {
#ifdef VERBOSE
        AUDIO_LOG("input_postprocess: Returning Volume:(%d) < iaxci_silence_threshold (%d)",volume, iaxci_silence_threshold);
#endif
        return volume < iaxci_silence_threshold;
    }
}

static int output_postprocess(void *audio, int len)
{
	calculate_level((short *)audio, len, &output_level);

	do_level_callback();

	return 0;
}

static struct iaxc_audio_codec *create_codec(int format)
{
	switch (format & IAXC_AUDIO_FORMAT_MASK)
	{
#ifdef CODEC_GSM
	case IAXC_FORMAT_GSM:
		return codec_audio_gsm_new();
#endif
	case IAXC_FORMAT_ULAW:
		return codec_audio_ulaw_new();
	case IAXC_FORMAT_ALAW:
		return codec_audio_alaw_new();
	case IAXC_FORMAT_SPEEX:
		return codec_audio_speex_new(&speex_settings);
#ifdef CODEC_ILBC
	case IAXC_FORMAT_ILBC:
		return codec_audio_ilbc_new();
#endif
	default:
		/* ERROR: codec not supported */
		fprintf(stderr, "ERROR: Codec not supported: %d\n", format);
		return NULL;
	}
}

EXPORT void iaxc_set_speex_settings(int decode_enhance, float quality,
		int bitrate, int vbr, int abr, int complexity)
{
	speex_settings.decode_enhance = decode_enhance;
	speex_settings.quality = quality;
	speex_settings.bitrate = bitrate;
	speex_settings.vbr = vbr;
	speex_settings.abr = abr;
	speex_settings.complexity = complexity;
}

// Audio quality preset constants
#define AUDIO_PRESET_STANDARD 0  // Standard balanced settings
#define AUDIO_PRESET_NOISY 1     // Better for noisy environments
#define AUDIO_PRESET_QUIET 2     // Better for quiet environments
#define AUDIO_PRESET_BANDWIDTH 3 // Lower bandwidth usage
#define AUDIO_PRESET_CUSTOM 99   // Custom user-defined settings

// Audio quality preset type
typedef int iaxc_audio_preset;

static iaxc_audio_preset current_audio_preset = AUDIO_PRESET_STANDARD;

// Function to switch between audio quality presets
EXPORT void iaxc_set_audio_preset(int preset_id)
{
    // Store the preset selection
    current_audio_preset = preset_id;
    
    // Apply changes to both codec and preprocessing settings
    switch (preset_id) {
        case AUDIO_PRESET_NOISY:
            // Noisy environment preset - aggressive noise reduction, moderate AGC
            AUDIO_LOG("Setting audio preset: NOISY ENVIRONMENT");
            
            // Speex codec settings
            speex_settings.decode_enhance = 1;
            speex_settings.quality = 6.0f;  // Medium quality (range 0-10)
            speex_settings.complexity = 4;  // Medium complexity for better noise handling
            
            // Filters to enable
            iaxci_filters = IAXC_FILTER_AGC | IAXC_FILTER_DENOISE | IAXC_FILTER_CN;
            
            // Set target level for normalization
            target_level = 0.75f;  // Higher target level for noisy environments
            break;
            
        case AUDIO_PRESET_QUIET:
            // Quiet environment preset - light noise reduction, gentle AGC
            AUDIO_LOG("Setting audio preset: QUIET ENVIRONMENT");
            
            // Speex codec settings
            speex_settings.decode_enhance = 1;
            speex_settings.quality = 8.0f;  // Higher quality (range 0-10)
            speex_settings.complexity = 5;  // Higher complexity for better quality
            
            // Filters to enable
            iaxci_filters = IAXC_FILTER_AGC | IAXC_FILTER_DENOISE | IAXC_FILTER_AAGC | IAXC_FILTER_CN;
            
            // Set target level for normalization
            target_level = 0.65f;  // Lower target level for quiet environments
            break;
            
        case AUDIO_PRESET_BANDWIDTH:
            // Low bandwidth preset - lower quality, more compression
            AUDIO_LOG("Setting audio preset: LOW BANDWIDTH");
            
            // Speex codec settings
            speex_settings.decode_enhance = 1;
            speex_settings.quality = 4.0f;  // Lower quality (range 0-10)
            speex_settings.complexity = 2;  // Lower complexity for less CPU usage
            
            // Filters to enable - minimal processing
            iaxci_filters = IAXC_FILTER_DENOISE | IAXC_FILTER_CN;
            
            // Set target level for normalization
            target_level = 0.7f;  // Medium target level
            break;
            
        case AUDIO_PRESET_STANDARD:
        default:
            // Standard preset - balanced settings
            AUDIO_LOG("Setting audio preset: STANDARD");
            
            // Speex codec settings
            speex_settings.decode_enhance = 1;
            speex_settings.quality = 7.0f;  // Good quality (range 0-10)
            speex_settings.complexity = 3;  // Standard complexity
            
            // Filters to enable
            iaxci_filters = IAXC_FILTER_AGC | IAXC_FILTER_DENOISE | IAXC_FILTER_AAGC | IAXC_FILTER_CN;
            
            // Set target level for normalization
            target_level = 0.7f;  // Standard target level
            break;
    }
    
    // Apply the new settings to the preprocessor states
    set_speex_filters();
    
    // Reset gain tracking for level normalization
    current_gain = 1.0f;
    current_level_peak = 0.1f;  // Start with a reasonable level
}

static int ptt_active=-1;
EXPORT void set_ptt(int val)
{
    ptt_active = val;
}
int audio_send_encoded_audio(struct iaxc_call *call, int callNo, void *data,
        int format, int sample_count)
{
    unsigned char outbuf[1024];
    int outsize = 1024;
    int insize = sample_count;
    static int was_silent_before = 0;
    
    /* update last input timestamp */
    timeLastInput = iax_tvnow();
    
    // Only record audio to WAV file - don't process it for silence detection
    if (audio_capture_file) {
        short *audio_data = (short *)data;
        // Add statistics gathering
        for (int i = 0; i < insize; i++) {
            if (audio_data[i] > audio_max_sample) audio_max_sample = audio_data[i];
            if (audio_data[i] < audio_min_sample) audio_min_sample = audio_data[i];
        }
        
        // Write raw PCM data to file
        size_t written = fwrite(data, sizeof(short), insize, audio_capture_file);
        if (written != insize) {
            AUDIO_LOG("audio_send_encoded_audio:WARNING: Failed to write all audio data: %zu/%d written", written, insize);
        }
        audio_samples_written += insize;
        audio_capture_frame_count++;
        fflush(audio_capture_file);
    }
    
    // Normalize audio levels for consistent transmission volume
    short *audio_samples = (short *)data;
    
    // Apply normalization to adjust levels for optimal clarity
    normalize_audio_buffer(audio_samples, insize);
    
    // Enhanced silence detection logic with voice onset detection
    int silent = 0;
    
    // Skip silence detection completely in PTT mode
    if(ptt_active>=0) {
#ifdef VERBOSE
        AUDIO_LOG("audio_send_encoded_audio:PTT active: BYPASSING SILENCE DETECTION");
#endif
    }
    else {
        // First, analyze initial portion for voice onset detection
        int max_sample = 0;
        int transient_count = 0;
        int prev_sample = 0;
        
        // Scan for voice onset markers (sharp transients, consistent level)
        for (int i = 0; i < (insize < 30 ? insize : 30); i++) {
            int abs_val = abs(audio_samples[i]);
            if (abs_val > max_sample) max_sample = abs_val;
            
            // Count significant transients (typical of voice onset)
            if (abs(audio_samples[i] - prev_sample) > 300) {
                transient_count++;
            }
            prev_sample = audio_samples[i];
        }
        
        // Standard silence detection with preprocessing
        silent = input_postprocess(data, insize, 8000);
        
        // Override for definite voice onset detected
        if (silent && ((max_sample > 2000) || (transient_count >= 3))) {
            //AUDIO_LOG("audio_send_encoded_audio:Voice onset detected, overriding VAD decision");
            silent = 0;  // Override silence detection on voice onset
        }
    }

    // Continue with regular IAX silence handling
    if(silent)
    {
        if(!call->tx_silent)
        {  // send a Comfort Noise Frame
            call->tx_silent = 1;
            if ( iaxci_filters & IAXC_FILTER_CN )
                iax_send_cng(call->session, 10, NULL, 0);
            AUDIO_LOG("audio_send_encoded_audio: Sent comfort SILENT frame and returnig");
        }

        return 0;  // skip encoding silent frames for network
    }

    /* we're going to send voice now */
    call->tx_silent = 0;
    
    /* destroy encoder if it is incorrect type */
    if(call->encoder && call->encoder->format != format)
    {
        call->encoder->destroy(call->encoder);
        call->encoder = NULL;
    }
    //AUDIO_LOG("Currently using format: 0x%x (must be not zero)", format);
    /* just break early if there's no format defined: this happens for the
     * first couple of frames of new calls */
    if(format == 0) return 0;

    /* create encoder if necessary */
    if(!call->encoder)
    {
        AUDIO_LOG("audio_send_encoded_audio:Creating encoder for format: 0x%x", format);
        call->encoder = create_codec(format);
        AUDIO_LOG("audio_send_encoded_audio:Encoder creation result: %s", call->encoder ? "SUCCESS" : "FAILED");
    }

    if(!call->encoder)
    {
        /* ERROR: no codec */
        fprintf(stderr, "ERROR: Codec could not be created: %d\n", format);
        AUDIO_LOG("audio_send_encoded_audio:ERROR: Codec could not be created: %d\n", format);
        return 0;
    }

    if(call->encoder->encode(call->encoder, &insize, (short *)data,
                &outsize, outbuf))
    {
        /* ERROR: codec error */
        fprintf(stderr, "ERROR: encode error: %d\n", format);
        return 0;
    }
#ifdef VERBOSE
    AUDIO_LOG("audio_send_encoded_audio:Encoded %d bytes of audio data", sizeof(outbuf) - outsize);
#endif
    // Send the encoded audio data back to the app if required
    if (iaxc_get_audio_prefs() & IAXC_AUDIO_PREF_RECV_LOCAL_ENCODED) {
#ifdef VERBOSE
        AUDIO_LOG("audio_send_encoded_audio:Sending local encoded audio back to app (size=%d)", outsize);
#endif
        iaxci_do_audio_callback(callNo, 0, IAXC_SOURCE_LOCAL, 1,
                call->encoder->format & IAXC_AUDIO_FORMAT_MASK,
                sizeof(outbuf) - outsize, outbuf);
    } else {
#ifdef VERBOSE
        AUDIO_LOG("Not sending local encoded audio back to app (not enabled)");
#endif
    }

    // Always send voice data regardless of callback preferences
    if(iax_send_voice(call->session, format, outbuf,
                sizeof(outbuf) - outsize, sample_count) == -1)
    {
        fprintf(stderr, "Failed to send voice! %s\n", iax_errstr);
        AUDIO_LOG("audio_send_encoded_audio:Failed to send voice! %s\n", iax_errstr);
        return -1;
    } else {
#ifdef VERBOSE
        AUDIO_LOG("audio_send_encoded_audio:Sent %d bytes of encoded audio data", sizeof(outbuf) - outsize);
#endif
    }

    return 0;
}

/* decode encoded audio; return the number of bytes decoded
 * negative indicates error */
int audio_decode_audio(struct iaxc_call * call, void * out, void * data, int len,
		int format, int * samples)
{
	int insize = len;
	int outsize = *samples;

	timeLastOutput = iax_tvnow();

	if ( format == 0 )
	{
		fprintf(stderr, "audio_decode_audio: Format is zero (should't happen)!\n");
		return -1;
	}

	/* destroy decoder if it is incorrect type */
	if ( call->decoder && call->decoder->format != format )
	{
		call->decoder->destroy(call->decoder);
		call->decoder = NULL;
	}

	/* create decoder if necessary */
	if ( !call->decoder )
	{
		call->decoder = create_codec(format);
	}

	if ( !call->decoder )
	{
		fprintf(stderr, "ERROR: Codec could not be created: %d\n",
				format);
		return -1;
	}

	if ( call->decoder->decode(call->decoder,
				&insize, (unsigned char *)data,
				&outsize, (short *)out) )
	{
		fprintf(stderr, "ERROR: decode error: %d\n", format);
		return -1;
	}

	output_postprocess(out, *samples - outsize);

	*samples = outsize;
	return len - insize;
}

EXPORT int iaxc_get_filters(void)
{
	return iaxci_filters;
}

EXPORT void iaxc_set_filters(int filters)
{
	iaxci_filters = filters;
	set_speex_filters();
}

EXPORT void iaxc_set_silence_threshold(float thr)
{
	iaxci_silence_threshold = thr;
	set_speex_filters();
}

// Update create_wav_file

static FILE* create_wav_file(int sample_rate) {
    char filename[256];
    time_t now;
    struct tm *timeinfo;
    char current_dir[MAX_PATH];
    
    // Get current working directory for diagnostics
    if (GetCurrentDirectoryA(MAX_PATH, current_dir)) {
        AUDIO_LOG("create_wav_file:Current working directory: %s", current_dir);
    }
    
    time(&now);
    timeinfo = localtime(&now);
    
    // Format: audio_capture_YYYYMMDD_HHMMSS.wav
    strftime(filename, sizeof(filename), "audio_capture_%Y%m%d_%H%M%S.wav", timeinfo);
    
    AUDIO_LOG("create_wav_file:Attempting to create file: %s", filename);
    FILE* file = fopen(filename, "wb");
    if (!file) {
        AUDIO_LOG("create_wav_file:ERROR: Failed to create audio capture file: %s (errno=%d: %s)", 
                filename, errno, strerror(errno));
        return NULL;
    }
    
    // Write placeholder WAV header (will be updated when file is closed)
    RiffHeader riff = {{'R','I','F','F'}, 0, {'W','A','V','E'}};
    FmtHeader fmt = {
        {'f','m','t',' '}, 
        16, 
        1,                         // PCM format
        1,                         // Mono
        sample_rate,               // Sample rate
        sample_rate * 2,           // Byte rate (sample_rate * num_channels * bits_per_sample/8)
        2,                         // Block align (num_channels * bits_per_sample/8)
        16                         // Bits per sample
    };
    DataHeader data = {{'d','a','t','a'}, 0};
    
    fwrite(&riff, sizeof(riff), 1, file);
    fwrite(&fmt, sizeof(fmt), 1, file);
    fwrite(&data, sizeof(data), 1, file);
    
    AUDIO_LOG("create_wav_file:Successfully created audio capture file: %s", filename);
    return file;
}

// Finalize WAV file by writing correct header sizes
static void finalize_wav_file(FILE* file, int data_size) {
    if (!file) return;
    
    // Calculate and update sizes in WAV header
    uint32_t data_chunk_size = data_size;
    uint32_t riff_chunk_size = 36 + data_chunk_size;
    
    AUDIO_LOG("finalize_wav_file:Finalizing WAV file with data_size=%d bytes", data_size);
    
    // Update RIFF chunk size
    fseek(file, 4, SEEK_SET);
    fwrite(&riff_chunk_size, 4, 1, file);
    
    // Update data chunk size
    fseek(file, 40, SEEK_SET);
    fwrite(&data_chunk_size, 4, 1, file);
    
    // Flush before closing
    fflush(file);
    
    // Close the file
    fclose(file);
    AUDIO_LOG("finalize_wav_file:Audio capture completed. Wrote %d bytes of audio data.", data_size);
}

// Function to start a new audio capture
EXPORT void iaxc_debug_audio_capture_start(void) {
    // Close any existing capture file
    if (audio_capture_file) {
        finalize_wav_file(audio_capture_file, audio_samples_written * 2);
        audio_capture_file = NULL;
    }
    
    audio_samples_written = 0;
    audio_capture_file = create_wav_file(audio_capture_sample_rate);
}

// Function to stop audio capture
EXPORT void iaxc_debug_audio_capture_stop(void) {
    if (audio_capture_file) {
        finalize_wav_file(audio_capture_file, audio_samples_written * 2);
        audio_capture_file = NULL;
        audio_samples_written = 0;
    }
}

// Function to start recording on PTT press
EXPORT void iaxc_ptt_audio_capture_start(void) {
    // Close any existing capture file
    if (audio_capture_file) {
        finalize_wav_file(audio_capture_file, audio_samples_written * 2);
        audio_capture_file = NULL;
        AUDIO_LOG("iaxc_ptt_audio_capture_start:Closed existing audio file before starting new one");
    }
    
    // Reset statistics
    audio_samples_written = 0;
    audio_capture_frame_count = 0;
    audio_max_sample = 0;
    audio_min_sample = 32767;
    audio_capture_start_time = time(NULL);
    
    // Create a PTT-specific filename format that includes "ptt" in the name
    char filename[MAX_PATH] = "";
    char exe_path[MAX_PATH] = "";
    char *last_slash = NULL;
    
    // Get the executable directory path
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) == 0) {
        AUDIO_LOG("iaxc_ptt_audio_capture_start:ERROR: Failed to get executable path (error=%d)", GetLastError());
        return;
    }
    
    // Find the last backslash to extract just the directory
    last_slash = strrchr(exe_path, '\\');
    if (last_slash != NULL) {
        *(last_slash + 1) = '\0';  // Truncate after the slash to get just the directory
    } else {
        exe_path[0] = '\0';
    }
    
    AUDIO_LOG("iaxc_ptt_audio_capture_start:Using executable directory: %s", exe_path);
    
    time_t now;
    struct tm *timeinfo;
    
    time(&now);
    timeinfo = localtime(&now);
    
    // Format: ptt_audio_YYYYMMDD_HHMMSS.wav in executable directory
    snprintf(filename, sizeof(filename), "%sptt_audio_%04d%02d%02d_%02d%02d%02d.wav", 
             exe_path,
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    AUDIO_LOG("iaxc_ptt_audio_capture_start:ATTEMPTING TO CREATE FILE: %s", filename);
    
    FILE* file = fopen(filename, "wb");
    if (!file) {
        AUDIO_LOG("iaxc_ptt_audio_capture_start:ERROR: Failed to create PTT audio capture file: %s (errno=%d: %s)", 
                 filename, errno, strerror(errno));
        return;
    }
    
    // Go back to 8000Hz for the WAV file since that's the actual sample rate
    const int fixed_sample_rate = 8000;  // Standard telephone quality
    
    AUDIO_LOG("iaxc_ptt_audio_capture_start:Creating WAV with sample rate: %d Hz", fixed_sample_rate);
    
    // Write placeholder WAV header (will be updated when file is closed)
    RiffHeader riff = {{'R','I','F','F'}, 0, {'W','A','V','E'}};
    FmtHeader fmt = {
        {'f','m','t',' '}, 
        16, 
        1,                    // PCM format
        1,                    // Mono
        fixed_sample_rate,    // Sample rate - 8000Hz matches OpenAL capture rate
        fixed_sample_rate * 2, // Byte rate (sample_rate * num_channels * bits_per_sample/8)
        2,                    // Block align
        16                    // Bits per sample
    };
    DataHeader data = {{'d','a','t','a'}, 0};
    
    fwrite(&riff, sizeof(riff), 1, file);
    fwrite(&fmt, sizeof(fmt), 1, file);
    fwrite(&data, sizeof(data), 1, file);
    
    audio_capture_file = file;
    AUDIO_LOG("iaxc_ptt_audio_capture_start:SUCCESS: Started PTT audio capture: %s (8000Hz sample rate)", filename);
    
    // Disable filters for better audio quality
    iaxc_ptt_filters_disable();
}

// Function to stop recording on PTT release
EXPORT void iaxc_ptt_audio_capture_stop(void) {
    if (audio_capture_file) {
        time_t end_time = time(NULL);
        double wall_clock_duration = difftime(end_time, audio_capture_start_time);
        double audio_duration = audio_samples_written / 8000.0;
        int bytes_written = audio_samples_written * 2;
        double bitrate = (bytes_written * 8) / audio_duration / 1000.0;
        
        // Finalize the WAV file
        finalize_wav_file(audio_capture_file, bytes_written);
        
        // Output comprehensive statistics
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:-------- AUDIO RECORDING STATISTICS --------");
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Total samples written: %d samples", audio_samples_written);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Number of frames processed: %d frames", audio_capture_frame_count);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Average frame size: %.1f samples", (float)audio_samples_written / audio_capture_frame_count);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Audio duration: %.2f seconds (at 8000 Hz)", audio_duration);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Wall clock duration: %.2f seconds", wall_clock_duration);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Speed ratio: %.2f (ideal = 1.0)", audio_duration / wall_clock_duration);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:File size: %d bytes", bytes_written);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Effective bitrate: %.1f kbps", bitrate);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:Dynamic range: min=%d, max=%d (peak=%.1f%%)", 
                 audio_min_sample, audio_max_sample, 
                 audio_max_sample * 100.0 / 32767.0);
        AUDIO_LOG("iaxc_ptt_audio_capture_stop:------------------------------------------");
        
        audio_capture_file = NULL;
        
        // Restore filters
        iaxc_ptt_filters_restore();
    }
    
    audio_samples_written = 0;
}

/* 
 * Handle PTT events triggered by text messages
 * This should be called from the IAX text message handler
 */
EXPORT void iaxc_handle_audio_event(const char* message) {
    if (!message) return;
#ifdef SAVE_LOCAL_AUDIO    
    if (strcmp(message, "Radio key pressed") == 0) {
        iaxc_ptt_audio_capture_start();
        fprintf(stderr, "Started audio recording on PTT press\n");
        AUDIO_LOG("iaxc_handle_audio_event:Starting audio recording on PTT press");        
    } else if (strcmp(message, "Radio key released") == 0) {
        iaxc_ptt_audio_capture_stop();
        fprintf(stderr, "Stopped audio recording on PTT release\n");
        AUDIO_LOG("iaxc_handle_audio_event:Stopping audio recording on PTT release");        
    }
#else
    AUDIO_LOG("iaxc_handle_audio_event:PTT audio recordings are disabled in this build. No action taken.");    
#endif    
}

void test_send_reference_tone(struct iaxc_call *call, int callNo) {
    // Generate 5 seconds of a 1kHz sine wave
    const int sample_rate = 8000;
    const int duration_sec = 5;
    const int total_samples = sample_rate * duration_sec;
    const float freq = 1000.0f;
    
    short *sine_wave = malloc(total_samples * sizeof(short));
    if (!sine_wave) return;
    
    // Generate sine wave
    for (int i = 0; i < total_samples; i++) {
        sine_wave[i] = (short)(10000.0f * sin(2.0f * M_PI * freq * i / sample_rate));
    }
    
    // Send in chunks of 160 samples (20ms)
    for (int i = 0; i < total_samples; i += 160) {
        int chunk_size = (i + 160 <= total_samples) ? 160 : (total_samples - i);
        audio_send_encoded_audio(call, callNo, &sine_wave[i], AST_FORMAT_SLINEAR, chunk_size);
        // Sleep for 20ms to simulate real-time
#ifdef _WIN32
        Sleep(20);  // Windows Sleep takes milliseconds
#else
        usleep(20000);  // POSIX usleep takes microseconds
#endif
    }
    
    free(sine_wave);
    AUDIO_LOG("test_send_reference_tone:Finished sending test tone");
}

// Temporarily disable/restore filters during PTT operations
static int saved_filters = 0;

EXPORT void iaxc_ptt_filters_disable(void) {
    // Save current filters and disable most processing
    saved_filters = iaxci_filters;
    // Keep only minimal necessary filters
    iaxc_set_filters(0);
    AUDIO_LOG("iaxc_ptt_filters_disable:PTT: Disabled audio filters for better voice quality");
}

EXPORT void iaxc_ptt_filters_restore(void) {
    // Restore previous filters
    iaxc_set_filters(saved_filters);
    AUDIO_LOG("iaxc_ptt_filters_restore:PTT: Restored audio filters to previous settings");
}

// Apply soft-clipping to prevent harsh digital clipping
static short soft_clip(float sample) {
    // Soft clipper with smooth transition
    if (sample > 32000.0f) {
        // Soft knee compression above 32000
        float excess = sample - 32000.0f;
        sample = 32000.0f + (1.0f - expf(-0.1f * excess / 768.0f)) * 768.0f;
    } else if (sample < -32000.0f) {
        // Soft knee compression below -32000
        float excess = -sample - 32000.0f;
        sample = -32000.0f - (1.0f - expf(-0.1f * excess / 768.0f)) * 768.0f;
    }
    
    // Hard limit at int16 boundaries to be safe
    if (sample > 32767.0f) sample = 32767.0f;
    if (sample < -32768.0f) sample = -32768.0f;
    
    return (short)sample;
}

// Apply normalization to a buffer of audio samples
static void normalize_audio_buffer(short *buffer, int samples) {
    float max_level = 0.0f;
    
    // Find peak level in this buffer
    for (int i = 0; i < samples; i++) {
        float abs_sample = fabsf((float)buffer[i] / 32768.0f);
        if (abs_sample > max_level) max_level = abs_sample;
    }
    
    // Update smoothed level detector
    current_level_peak = level_smoothing * current_level_peak + 
                        (1.0f - level_smoothing) * max_level;
    
    // Calculate target gain if signal is present
    if (current_level_peak > 0.01f) { // Only adjust gain when signal is present
        float target_gain = target_level / current_level_peak;
        
        // Limit maximum gain to prevent noise amplification
        if (target_gain > 4.0f) target_gain = 4.0f;
        
        // Smooth gain changes to prevent artifacts
        current_gain = gain_smoothing * current_gain + 
                      (1.0f - gain_smoothing) * target_gain;
    }
    
    // Apply gain and soft clipping to each sample
    for (int i = 0; i < samples; i++) {
        float processed = (float)buffer[i] * current_gain;
        buffer[i] = soft_clip(processed);
    }
}

