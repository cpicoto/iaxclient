#include "iaxclient_lib.h"

#ifdef __APPLE__
# include <OpenAL/al.h>
# include <OpenAL/alc.h>
#elif defined(OPENALSDK)
# include <al.h>
# include <alc.h>
#else
# include "AL/al.h"
# include "AL/alc.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #define OPENAL_LOG(fmt, ...)                                                    \
    do {                                                                           \
      char _buf[512];                                                              \
      char _time_buf[32];                                                          \
      SYSTEMTIME _st;                                                              \
      GetLocalTime(&_st);                                                          \
      snprintf(_time_buf, sizeof(_time_buf), "%02d:%02d:%02d.%03d",                \
               _st.wHour, _st.wMinute, _st.wSecond, _st.wMilliseconds);            \
      _snprintf(_buf, sizeof(_buf), "%s:[openal-debug] " fmt "\n",                 \
               _time_buf, ##__VA_ARGS__);                                          \
      OutputDebugStringA(_buf);                                                    \
    } while(0)
#else
  #include <time.h>
  #include <sys/time.h>
  #define OPENAL_LOG(fmt, ...)                                                    \
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
      fprintf(stderr, "[openal-debug %s] " fmt "\n", _time_buf, ##__VA_ARGS__);    \
    } while(0)
#endif

struct openal_priv_data {
    int sample_rate;

    /* playback buffers */
    int num_buffers, buffers_head, buffers_tail, buffers_free;
    ALuint *buffers;
    ALuint source;

    /* OpenAL contexts & devices */
    ALCcontext *out_ctx;
    ALCdevice  *in_dev;

    /* volume control */
    float input_level, output_level;

    /* device enumeration */
    const ALCchar **captureDevices;
    int numCapture, selectedCapture;
    const ALCchar **playDevices;
    int numPlay, selectedPlay;

    /* IAXClient device array */
    struct iaxc_audio_device *devices;

    // Multi-device support (Windows)
    #ifdef _WIN32
    struct {
        ALCdevice *device;
        ALCcontext *context;
        ALuint source;
        int active;
    } device_contexts[16]; // Support up to 16 simultaneous devices
    int active_device_index;
    #endif
};

/* single dummy entry (overwritten in initialize) */
static struct iaxc_audio_device device = {
    "default",
    IAXC_AD_INPUT  | IAXC_AD_INPUT_DEFAULT |
    IAXC_AD_OUTPUT | IAXC_AD_OUTPUT_DEFAULT |
    IAXC_AD_RING   | IAXC_AD_RING_DEFAULT,
    0
};
#define MAX_SOUNDS 32
#define OPENAL_BUFFER_COUNT 32

// Declare sound sources structure
static struct {
    ALuint buffer;
    ALuint source;
    int id;
    int playing;
} sound_sources[MAX_SOUNDS];
static int sounds_initialized = 0;

// Global driver reference
static struct iaxc_audio_driver* current_audio_driver = NULL;

// Forward declarations for functions
static int openal_destroy(struct iaxc_audio_driver *d);
int openal_mic_boost_get(struct iaxc_audio_driver *d);
int openal_mic_boost_set(struct iaxc_audio_driver *d, int enable);
int openal_play_sound(struct iaxc_sound *s, int ring);
int openal_stop_sound(int id);
void openal_diagnostic(struct iaxc_audio_driver *d);
static void validate_source_state(struct openal_priv_data* priv);
static void verify_audio_device(struct openal_priv_data* priv);
static void handle_openal_error(const char* operation);
static void check_device_health(struct openal_priv_data* priv);
#ifdef _WIN32
static int force_audio_restart(struct iaxc_audio_driver *d, int output);
#endif

static int openal_error(const char* fn, int err) {
    fprintf(stderr, "[openal] %s failed (0x%X)\n", fn, err);
    return -1;
}

int openal_input(struct iaxc_audio_driver *d, void *samples, int *nSamples) {
    struct openal_priv_data* priv = d->priv;
    ALCint available;
    
    // Check if capture device is valid
    if (!priv->in_dev) {
        OPENAL_LOG("ERROR: No capture device available");
        *nSamples = 0;
        return -1;
    }
    
    alcGetIntegerv(priv->in_dev, ALC_CAPTURE_SAMPLES, sizeof(available), &available);
    
    // Log if we're consistently getting zero samples
    static int zero_count = 0;
    if (available == 0) {
        if (++zero_count % 100 == 0) {
            OPENAL_LOG("WARNING: No samples available for capture (count: %d)", zero_count);
        }
    } else {
        zero_count = 0;
    }

    int req = (available < *nSamples) ? available : *nSamples;
    if (req > 0) {
        short *sampleBuf = (short*)samples;
        alcCaptureSamples(priv->in_dev, samples, req);
        
        // Log audio levels periodically to confirm real data is coming in
        static int level_check = 0;
        if (++level_check % 100 == 0) {
            short max_level = 0;
            for (int i = 0; i < req; i++) {
                short abs_val = abs(sampleBuf[i]);
                if (abs_val > max_level) max_level = abs_val;
            }
            /*
            OPENAL_LOG("Audio input level: %d/32767 (%d%%)", 
                      max_level, (max_level * 100) / 32767);
                      */
        }
        
    }
    
    *nSamples = req;
    return 0;
}

static void openal_unqueue(struct openal_priv_data* priv) {
    ALint processed;
    alGetSourcei(priv->source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint b;
        alSourceUnqueueBuffers(priv->source, 1, &b);
        priv->buffers[priv->buffers_tail++] = b;
        if (priv->buffers_tail >= priv->num_buffers)
            priv->buffers_tail = 0;
        priv->buffers_free++;
    }
}

int openal_output(struct iaxc_audio_driver *d, void *samples, int nSamples) {
    struct openal_priv_data* priv = d->priv;
    ALCenum error;
    ALint st;
    
#ifdef _WIN32
    // For Windows, use the active device from our device_contexts array
    int active_idx = priv->active_device_index;
    if (active_idx >= 0 && priv->device_contexts[active_idx].active) {
        // Switch to the active device's context
        alcMakeContextCurrent(priv->device_contexts[active_idx].context);
        
        // Use the device's source
        priv->source = priv->device_contexts[active_idx].source;
        
        // Track the context for other functions
        priv->out_ctx = priv->device_contexts[active_idx].context;
    }
#endif
    
    // Safety checks
    if (!priv->out_ctx) {
        OPENAL_LOG("No output context available");
        return -1;
    }
    
    // Make context current with better error handling
    if (alcMakeContextCurrent(priv->out_ctx) == ALC_FALSE) {
        OPENAL_LOG("Error making context current");
        return -1;
    }
    
    // Check for state corruption
    validate_source_state(priv);
    
    // Rest of function remains the same...
    // Process finished buffers before allocating new ones
    openal_unqueue(priv);
    
    // Enhanced timeout handling to avoid getting stuck
    int timeout = 100;  // 100ms timeout
    while (priv->buffers_free == 0 && timeout > 0) {
        iaxc_millisleep(5);
        openal_unqueue(priv);
        timeout -= 5;
    }
    
    // More aggressive buffer reclamation
    if (priv->buffers_free == 0) {
        OPENAL_LOG("Buffer starvation - forcing buffer reclaim");
        alSourceStop(priv->source);
        alSourceRewind(priv->source);  // Try to reset the source completely
        openal_unqueue(priv);
        
        if (priv->buffers_free == 0) {
            // If we're still out of buffers, recreate the entire queue
            OPENAL_LOG("Emergency buffer reset");
            
            // Delete old source and buffers
            alDeleteSources(1, &priv->source);
            alDeleteBuffers(priv->num_buffers, priv->buffers);
            
            // Create new ones
            alGenSources(1, &priv->source);
            alGenBuffers(priv->num_buffers, priv->buffers);
            priv->buffers_free = priv->num_buffers;
            priv->buffers_head = priv->buffers_tail = 0;
        }
    }

    ALuint buf = priv->buffers[priv->buffers_head++];
    if (priv->buffers_head >= priv->num_buffers)
        priv->buffers_head = 0;

    alBufferData(buf, AL_FORMAT_MONO16, samples, nSamples * sizeof(short), priv->sample_rate);
    ALenum err = alGetError();
    if (err != AL_NO_ERROR) {
        OPENAL_LOG("alBufferData failed: 0x%X", err);
        return -1;
    }

    alSourceQueueBuffers(priv->source, 1, &buf);
    priv->buffers_free--;

    err = alGetError();
    if (err != AL_NO_ERROR) {
        OPENAL_LOG("alSourceQueueBuffers failed: 0x%X", err);
        return -1;
    }

    // Modify this condition to ensure the source is always playing when we have data:
    if (priv->buffers_free < priv->num_buffers) {  // If any buffers are queued
        ALint st;
        alGetSourcei(priv->source, AL_SOURCE_STATE, &st);
        
        // Only start if not already playing and avoid excessive logging
        if (st != AL_PLAYING) {
            static int last_report = 0;
            int now = GetTickCount();  // Windows specific, use appropriate timer elsewhere
            
            alSourcePlay(priv->source);
#ifdef NOTQUIET            
            // Only log with reasonable frequency
            if (now - last_report > 1000) {  // Once per second max
                OPENAL_LOG("Source started playing");
                last_report = now;
            }
#endif
        }
    }

    // Add this for periodic health checks
    static int health_check = 0;
    if (++health_check % 100 == 0) {
        ALint state, processed, queued;
        alGetSourcei(priv->source, AL_SOURCE_STATE, &state);
        alGetSourcei(priv->source, AL_BUFFERS_PROCESSED, &processed);
        alGetSourcei(priv->source, AL_BUFFERS_QUEUED, &queued);
        /*
        OPENAL_LOG("Audio health: State=%d, Processed=%d, Queued=%d, Free=%d",
                  state, processed, queued, priv->buffers_free);
        */
        if (state == AL_STOPPED && queued > 0) {
            OPENAL_LOG("Source stopped unexpectedly, restarting");
            alSourcePlay(priv->source);
        }
    }
    alGetSourcei(priv->source, AL_SOURCE_STATE, &st);
    static int lastState = -1;
    if (st != lastState) {
        OPENAL_LOG("Source state changed: %d -> %d", lastState, st);
        lastState = st;
    }
    verify_audio_device(priv);
    return 0;
}

int openal_select_devices(struct iaxc_audio_driver *d, int input, int output, int ring) {
    struct openal_priv_data* priv = d->priv;
    ALCenum error; // Add this declaration
    
    // Input device switching looks good, keep as-is
    if (input >= 0 && input < priv->numCapture && input != priv->selectedCapture) {
        OPENAL_LOG("Switching capture device: %d -> %d", priv->selectedCapture, input);
        
        // Store old device for fallback
        ALCdevice *oldDev = priv->in_dev;
        int oldCaptureIndex = priv->selectedCapture;
        
        // Try to open new device first before closing old one
        ALCdevice *newDev = alcCaptureOpenDevice(
            priv->captureDevices[input],
            priv->sample_rate,
            AL_FORMAT_MONO16,
            priv->sample_rate 
        );
        
        if (!newDev) {
            error = alcGetError(NULL);
            OPENAL_LOG("Failed to open capture device: error 0x%X", error);
            OPENAL_LOG("Keeping current device");
            return -1;
        }
        
        // Stop old device
        alcCaptureStop(oldDev);
        alcCaptureCloseDevice(oldDev);
        
        // Update to new device
        priv->selectedCapture = input;
        priv->in_dev = newDev;
        alcCaptureStart(priv->in_dev);
    }

    /* Force playback device switch with complete context recreation */
    if (output >= 0 && output < priv->numPlay && output != priv->selectedPlay) {
        OPENAL_LOG("Switching output device: %d -> %d", priv->selectedPlay, output);

#ifdef _WIN32
        // On Windows, use our special restart approach
        return force_audio_restart(d, output);
#else
        // Your non-Windows code here
        // ...existing non-Windows code...
#endif  // Add this line to close the #ifdef _WIN32
    }
    (void)ring;
    return 0;
}

int openal_selected_devices(struct iaxc_audio_driver *d, int *input, int *output, int *ring) {
    struct openal_priv_data* priv = d->priv;
    *input  = priv->selectedCapture;
    *output = priv->selectedPlay;
    *ring   = 0;
    return 0;
}

int openal_start(struct iaxc_audio_driver *d) {
    struct openal_priv_data* priv = d->priv;
    
    // Ensure output context is current
    alcMakeContextCurrent(priv->out_ctx);
    
    // Restart capture device
    alcCaptureStop(priv->in_dev);
    alcCaptureStart(priv->in_dev);
    
    // Make sure source is playing
    ALint state;
    alGetSourcei(priv->source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) {
        alSourcePlay(priv->source);
    }
    
    // OPENAL_LOG("Audio streaming started");
    return 0;
}

int openal_stop(struct iaxc_audio_driver *d) {
    struct openal_priv_data* priv = d->priv;
    
    // Stop capture
    alcCaptureStop(priv->in_dev);
    
    // Stop playback
    alcMakeContextCurrent(priv->out_ctx);
    alSourceStop(priv->source);
    
    // Clear any queued buffers
    ALint queued;
    alGetSourcei(priv->source, AL_BUFFERS_QUEUED, &queued);
    while (queued--) {
        ALuint buffer;
        alSourceUnqueueBuffers(priv->source, 1, &buffer);
    }
    
    //OPENAL_LOG("Audio streaming stopped");
    return 0;
}

float openal_input_level_get(struct iaxc_audio_driver *d) {
    return ((struct openal_priv_data*)d->priv)->input_level;
}
float openal_output_level_get(struct iaxc_audio_driver *d) {
    return ((struct openal_priv_data*)d->priv)->output_level;
}
int openal_input_level_set(struct iaxc_audio_driver *d, float lvl) {
    struct openal_priv_data* priv = d->priv;
    // Use a smoother scale instead of binary on/off
    priv->input_level = lvl;
    OPENAL_LOG("Input level set to %.2f", lvl);
    return 0;
}
int openal_output_level_set(struct iaxc_audio_driver *d, float lvl) {
    struct openal_priv_data* priv = d->priv;
    priv->output_level = lvl;
    alSourcef(priv->source, AL_GAIN, lvl);
    return 0;
}

int openal_mic_boost_get(struct iaxc_audio_driver *d) {
    struct openal_priv_data* priv = d->priv;
    return (priv->input_level > 1.0f) ? 1 : 0;
}

int openal_mic_boost_set(struct iaxc_audio_driver *d, int enable) {
    struct openal_priv_data* priv = d->priv;
    if (enable) {
        priv->input_level = 2.0f;  // Boost by doubling gain
    } else if (priv->input_level > 1.0f) {
        priv->input_level = 1.0f;  // Return to normal
    }
    return 0;
}

int openal_play_sound(struct iaxc_sound *s, int ring) {
    // Store a reference to the driver
    struct iaxc_audio_driver *d = current_audio_driver;
    if (!d) return -1;
    
    struct openal_priv_data* priv = d->priv;
    
    // Make context current
    alcMakeContextCurrent(priv->out_ctx);
    
    // Initialize sound sources if needed
    if (!sounds_initialized) {
        memset(sound_sources, 0, sizeof(sound_sources));
        for (int i = 0; i < MAX_SOUNDS; i++) {
            alGenBuffers(1, &sound_sources[i].buffer);
            alGenSources(1, &sound_sources[i].source);
            sound_sources[i].id = -1;
            sound_sources[i].playing = 0;
        }
        sounds_initialized = 1;
    }
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (!sound_sources[i].playing) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        OPENAL_LOG("No free sound slots available");
        return -1;
    }
    
    // Configure source
    sound_sources[slot].id = s->id;
    sound_sources[slot].playing = 1;
    
    // Set up buffer with sound data - use the global sample rate
    alBufferData(sound_sources[slot].buffer, 
                AL_FORMAT_MONO16, 
                s->data, 
                s->len * sizeof(short), 
                priv->sample_rate); 
    
    // Attach buffer to source and play
    alSourcei(sound_sources[slot].source, AL_BUFFER, sound_sources[slot].buffer);
    alSourcei(sound_sources[slot].source, AL_LOOPING, s->repeat ? AL_TRUE : AL_FALSE);
    alSourcef(sound_sources[slot].source, AL_GAIN, priv->output_level);
    alSourcePlay(sound_sources[slot].source);
    
    return s->id;
}

int openal_stop_sound(int id) {
    // Find the sound with matching ID
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (sound_sources[i].playing && sound_sources[i].id == id) {
            alSourceStop(sound_sources[i].source);
            sound_sources[i].playing = 0;
            sound_sources[i].id = -1;
            return 0;
        }
    }
    return -1;
}

// Add a global error tracking function
static void handle_openal_error(const char* operation) {
    ALCenum error = alGetError();
    if (error != AL_NO_ERROR) {
        OPENAL_LOG("OpenAL error during %s: %s (0x%X)", 
                  operation, alGetString(error), error);
    }
}



// Add this new function somewhere after other utility functions
static void verify_audio_device(struct openal_priv_data* priv) {
    static int counter = 0;
    if (++counter % 100 != 0) return;  // Check more frequently
    
    ALCcontext *ctx = alcGetCurrentContext();
    if (!ctx) {
        OPENAL_LOG("WARNING: No current OpenAL context!");
        return;
    }
    
    ALCdevice *dev = alcGetContextsDevice(ctx);
    if (!dev) {
        OPENAL_LOG("WARNING: Cannot get device from current context!");
        return;
    }
    
#ifdef _WIN32
    // For Windows, compare to our active device
    int active_idx = priv->active_device_index;
    if (active_idx >= 0 && priv->device_contexts[active_idx].active) {
        // Check that the current context matches our active device
        if (ctx != priv->device_contexts[active_idx].context) {
            OPENAL_LOG("WARNING: Context mismatch! Expected: %p, Actual: %p", 
                      priv->device_contexts[active_idx].context, ctx);
            
            // Try to fix by switching back
            alcMakeContextCurrent(priv->device_contexts[active_idx].context);
        }
    }
#endif
    
    // On some implementations, we can get the device name
    const ALCchar* devName = NULL;
    if (alcIsExtensionPresent(dev, "ALC_ENUMERATE_ALL_EXT")) {
        devName = alcGetString(dev, ALC_ALL_DEVICES_SPECIFIER);
        //OPENAL_LOG("Currently using device: %s", devName ? devName : "unknown");
    }
}

static void check_device_health(struct openal_priv_data* priv) {
    static int counter = 0;
    
    // Only check every 100 calls to avoid overhead
    if (++counter % 100 != 0)
        return;
        
    // Check if output device is still valid
    if (priv->out_ctx) {
        ALCdevice *dev = alcGetContextsDevice(priv->out_ctx);
        
        // First check if the extension is present
        if (!alcIsExtensionPresent(dev, "ALC_EXT_disconnect")) {
            // Can't check device status on this implementation
            return;
        }
        
        // Extension is present, let's get the enumeration value dynamically
        // This avoids using the undefined constant
        static ALCint connected_enum = -1;
        
        // Get the enumeration value only once
        if (connected_enum == -1) {
            // Try to get the enum value - some implementations expose it differently
            const char* enum_names[] = {
                "ALC_CONNECTED", 
                "AL_CONNECTED", 
                "ALC_DEVICE_CONNECTED"
            };
            
            for (int i = 0; i < sizeof(enum_names)/sizeof(enum_names[0]); i++) {
                ALCenum value = alcGetEnumValue(dev, enum_names[i]);
                if (value != 0 && value != -1) {
                    connected_enum = value;
                    OPENAL_LOG("Found disconnection enum: %s = %d", enum_names[i], value);
                    break;
                }
            }
            
            if (connected_enum == -1) {
                OPENAL_LOG("Could not find disconnection enum value");
                return;
            }
        }
        
        // Now we can safely check the connection status
        ALCint connected = 0;
        alcGetIntegerv(dev, connected_enum, 1, &connected);
        
        if (!connected) {
            OPENAL_LOG("Output device disconnected - attempting to recover");
            // Try to reopen the default device
            ALCdevice *newDev = alcOpenDevice(NULL);
            if (newDev) {
                ALCcontext *newCtx = alcCreateContext(newDev, NULL);
                if (newCtx) {
                    // Save old context for cleanup
                    ALCcontext *oldCtx = priv->out_ctx;
                    
                    // Activate new context
                    alcMakeContextCurrent(newCtx);
                    priv->out_ctx = newCtx;
                    
                    // Clean up old context
                    alcMakeContextCurrent(NULL);
                    alcDestroyContext(oldCtx);
                    
                    OPENAL_LOG("Successfully recovered by creating new context");
                }
            }
        }
    }
}

static void validate_source_state(struct openal_priv_data* priv) {
    if (!priv || !priv->source) return;
    
    // Get the state without validating it immediately
    ALint state = 0;
    alGetSourcei(priv->source, AL_SOURCE_STATE, &state);
    
    // Only report unusual states
    if (state != AL_INITIAL && state != AL_PLAYING && 
        state != AL_PAUSED && state != AL_STOPPED) {
        
        static int last_reset = 0;
        int now = GetTickCount();
        
        // Only attempt fixes at most once per second
        if (now - last_reset > 1000) {
            OPENAL_LOG("FIXING: Invalid state %d detected - recreating source", state);
            last_reset = now;
            
            // Hard reset: Delete and recreate the source
            ALuint oldSource = priv->source;
            alSourceStop(oldSource);
            alSourcei(oldSource, AL_BUFFER, 0);  // Detach all buffers
            
            // Create new source
            ALuint newSource;
            alGenSources(1, &newSource);
            
            if (alGetError() == AL_NO_ERROR) {
                // Successfully created new source
                priv->source = newSource;
                alSourcef(priv->source, AL_GAIN, priv->output_level);
                
                // Clean up old source
                alDeleteSources(1, &oldSource);
                
                // Reset buffer tracking
                priv->buffers_free = priv->num_buffers;
                priv->buffers_head = priv->buffers_tail = 0;
            }
        }
    }
}

void openal_diagnostic(struct iaxc_audio_driver *d) {
    struct openal_priv_data* priv = d->priv;
    
    OPENAL_LOG("=== OPENAL DIAGNOSTIC ===");
    
    // Check context
    ALCcontext *ctx = alcGetCurrentContext();
    OPENAL_LOG("Current context: %p (expected: %p)", ctx, priv->out_ctx);
    
    // Check device
    ALCdevice *dev = alcGetContextsDevice(ctx);
    OPENAL_LOG("Current device: %p", dev);
    
    // Check source state
    const char* stateNames[] = {
        "unknown",
        "initial",   // 4096 (AL_INITIAL)
        "playing",   // 4097 (AL_PLAYING)
        "paused",    // 4098 (AL_PAUSED)
        "stopped"    // 4099 (AL_STOPPED)
    };

    ALint rawState;
    alGetSourcei(priv->source, AL_SOURCE_STATE, &rawState);

    const char* stateName = "invalid";
    if (rawState >= AL_INITIAL && rawState <= AL_STOPPED) {
        stateName = stateNames[rawState - AL_INITIAL + 1];
    }

    OPENAL_LOG("Source state: %d (%s)", rawState, stateName);
    
    // Check buffer state
    ALint processed, queued;
    alGetSourcei(priv->source, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(priv->source, AL_BUFFERS_QUEUED, &queued);
    OPENAL_LOG("Buffers: processed=%d, queued=%d, free=%d", 
              processed, queued, priv->buffers_free);
    
    // Check volume levels
    OPENAL_LOG("Levels: input=%.2f, output=%.2f", 
              priv->input_level, priv->output_level);
    
    // Check for any errors
    ALenum error = alGetError();
    OPENAL_LOG("Current error state: 0x%X", error);
    
    OPENAL_LOG("=== END DIAGNOSTIC ===");
}

int openal_destroy(struct iaxc_audio_driver *d) {
    struct openal_priv_data* priv = d->priv;
    
    // Stop audio first
    openal_stop(d);

#ifdef _WIN32
    // Clean up all device contexts
    for (int i = 0; i < 16; i++) {
        if (priv->device_contexts[i].active) {
            // Delete sources
            if (priv->device_contexts[i].source) {
                alcMakeContextCurrent(priv->device_contexts[i].context);
                alSourceStop(priv->device_contexts[i].source);
                alDeleteSources(1, &priv->device_contexts[i].source);
            }
            
            // Destroy context
            if (priv->device_contexts[i].context) {
                alcMakeContextCurrent(NULL);
                alcDestroyContext(priv->device_contexts[i].context);
            }
            
            // Close device
            if (priv->device_contexts[i].device) {
                alcCloseDevice(priv->device_contexts[i].device);
            }
            
            priv->device_contexts[i].active = 0;
        }
    }
#endif

    // Rest of the original destroy function...
    // Clean up capture device
    if (priv->in_dev) {
        alcCaptureCloseDevice(priv->in_dev);
        priv->in_dev = NULL;
    }
    
    // Clean up playback devices and contexts
    if (priv->out_ctx) {
        alcMakeContextCurrent(NULL);
        alcDestroyContext(priv->out_ctx);
        priv->out_ctx = NULL;
    }
    
    // Clean up buffers
    if (priv->buffers) {
        free(priv->buffers);
        priv->buffers = NULL;
    }
    
    // Clean up device lists
    if (priv->captureDevices) {
        free(priv->captureDevices);
        priv->captureDevices = NULL;
    }
    
    if (priv->playDevices) {
        free(priv->playDevices);
        priv->playDevices = NULL;
    }
    
    // Clean up device array
    if (priv->devices) {
        free(priv->devices);
        priv->devices = NULL;
    }
    
    // Finally, free the private data structure
    free(priv);
    
    return 0;
}

int openal_initialize(struct iaxc_audio_driver *d, int sample_rate) {
    struct openal_priv_data* priv = calloc(1, sizeof(*priv));
    if (!priv) return -1;
    d->priv = priv;
    priv->sample_rate = sample_rate;

    /* ensure playback context */
    ALCcontext *ctx = alcGetCurrentContext();
    if (!ctx) {
        ALCdevice *dev = alcOpenDevice(NULL);
        if (!dev) return openal_error("alcOpenDevice", alcGetError(NULL));
        ctx = alcCreateContext(dev, NULL);
        if (!ctx) return openal_error("alcCreateContext", alcGetError(NULL));
        alcMakeContextCurrent(ctx);
    }
    priv->out_ctx = ctx;

    /* enumerate capture devices */
    const ALCchar *cList = alcGetString(NULL, ALC_CAPTURE_DEVICE_SPECIFIER);
    for (const ALCchar *p = cList; *p; p += strlen(p) + 1) priv->numCapture++;
    priv->captureDevices = calloc(priv->numCapture, sizeof(*priv->captureDevices));
    { const ALCchar *p = cList;
      for (int i = 0; i < priv->numCapture; ++i) {
        priv->captureDevices[i] = p;
        p += strlen(p) + 1;
      }
    }
    OPENAL_LOG("Found %d capture devices:", priv->numCapture);
    for (int i = 0; i < priv->numCapture; ++i)
      OPENAL_LOG("  cap[%2d]: %s", i, priv->captureDevices[i]);

    /* enumerate playback devices */
    //const ALCchar *pList = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
    const ALCchar *pList = NULL;
    if (alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT")) {
        OPENAL_LOG("Using ALC_ALL_DEVICES_SPECIFIER to enumerate playback");
        pList = alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
    } else {
        OPENAL_LOG("Falling back to ALC_DEVICE_SPECIFIER (only default)");
        pList = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
    }
    for (const ALCchar *p = pList; *p; p += strlen(p) + 1) priv->numPlay++;
    priv->playDevices = calloc(priv->numPlay, sizeof(*priv->playDevices));
    { const ALCchar *p = pList;
      for (int i = 0; i < priv->numPlay; ++i) {
        priv->playDevices[i] = p;
        p += strlen(p) + 1;
      }
    }
    OPENAL_LOG("Found %d playback devices:", priv->numPlay);
    for (int i = 0; i < priv->numPlay; ++i)
      OPENAL_LOG("  play[%2d]: %s", i, priv->playDevices[i]);

    /* open default capture */
    priv->selectedCapture = 0;
    OPENAL_LOG("Attempting to open capture device '%s' with format: MONO16, rate: %d", 
              priv->captureDevices[0], sample_rate);

    priv->in_dev = alcCaptureOpenDevice(
        priv->captureDevices[0],
        sample_rate,
        AL_FORMAT_MONO16,
        sample_rate/2
    );

    if (!priv->in_dev) {
        ALCenum err = alcGetError(NULL);
        OPENAL_LOG("ERROR: Failed to open capture device: 0x%X", err);
        
        // Try with different format or sample rate as fallback
        OPENAL_LOG("Trying fallback: 16kHz sample rate");
        priv->in_dev = alcCaptureOpenDevice(
            priv->captureDevices[0],
            16000,  // Try lower sample rate
            AL_FORMAT_MONO16,
            8000    // Half the buffer size
        );
        
        if (!priv->in_dev) {
            return openal_error("alcCaptureOpenDevice (fallback)", alcGetError(NULL));
        }
        
        // Update internal sample rate if using fallback
        priv->sample_rate = 16000;
        OPENAL_LOG("Using fallback sample rate: 16kHz");
    }

    // Start capturing immediately
    alcCaptureStart(priv->in_dev);
    OPENAL_LOG("Capture device opened and started: '%s'", priv->captureDevices[0]);

    // Store the driver reference globally for sound playback
    current_audio_driver = d;
    
    // Prepare playback buffers - use a configurable amount
    priv->num_buffers = OPENAL_BUFFER_COUNT;
    priv->buffers = malloc(priv->num_buffers * sizeof(ALuint));
    if (!priv->buffers) {
        OPENAL_LOG("Failed to allocate buffer memory");
        return -1;
    }

    // Make sure context is current before generating buffers
    if (alcMakeContextCurrent(priv->out_ctx) == ALC_FALSE) {
        OPENAL_LOG("ERROR: Failed to make context current before generating buffers");
        free(priv->buffers);
        return -1;
    }

    // Generate buffers with error checking
    alGenBuffers(priv->num_buffers, priv->buffers);
    ALenum err = alGetError();
    if (err != AL_NO_ERROR) {
        OPENAL_LOG("ERROR: Failed to generate buffers: 0x%X", err);
        free(priv->buffers);
        return -1;
    }

    // Generate source with error checking
    alGenSources(1, &priv->source);
    err = alGetError();
    if (err != AL_NO_ERROR) {
        OPENAL_LOG("ERROR: Failed to generate source: 0x%X", err);
        // Clean up the buffers we already created
        alDeleteBuffers(priv->num_buffers, priv->buffers);
        free(priv->buffers);
        return -1;
    }

    // Set initial source properties
    alSourcef(priv->source, AL_GAIN, 1.0f);  // Default gain
    err = alGetError();
    if (err != AL_NO_ERROR) {
        OPENAL_LOG("WARNING: Could not set source properties: 0x%X", err);
    }

    // Initialize volume levels
    priv->input_level = 1.0f;
    priv->output_level = 1.0f;
    priv->buffers_free = priv->num_buffers;

    /* build IAXClient device list */
    int total = priv->numCapture + priv->numPlay;
    d->nDevices = total;
    priv->devices = malloc(total * sizeof(*priv->devices));
    for (int i = 0; i < priv->numCapture; ++i) {
        priv->devices[i].name         = priv->captureDevices[i];
        priv->devices[i].capabilities = IAXC_AD_INPUT | IAXC_AD_INPUT_DEFAULT;
        priv->devices[i].devID        = i;
    }
    for (int i = 0; i < priv->numPlay; ++i) {
        int idx = priv->numCapture + i;
        priv->devices[idx].name         = priv->playDevices[i];
        priv->devices[idx].capabilities = IAXC_AD_OUTPUT|IAXC_AD_OUTPUT_DEFAULT
                                        |IAXC_AD_RING  |IAXC_AD_RING_DEFAULT;
        priv->devices[idx].devID        = idx;
    }
    d->devices = priv->devices;

    /* hook callbacks */
    d->initialize       = openal_initialize;
    d->destroy          = openal_destroy;
    d->select_devices   = openal_select_devices;
    d->selected_devices = openal_selected_devices;
    d->start            = openal_start;
    d->stop             = openal_stop;
    d->output           = openal_output;
    d->input            = openal_input;
    d->input_level_get  = openal_input_level_get;
    d->input_level_set  = openal_input_level_set;
    d->output_level_get = openal_output_level_get;
    d->output_level_set = openal_output_level_set;
    d->mic_boost_get    = openal_mic_boost_get;
    d->mic_boost_set    = openal_mic_boost_set;
    d->play_sound       = openal_play_sound;
    d->stop_sound       = openal_stop_sound;

    OPENAL_LOG("Registering %d IAX devices (cap+play)", total);
    for (int i = 0; i < total; ++i) {
        OPENAL_LOG("  dev[%2d]: name='%s' caps=0x%02X",
                   priv->devices[i].devID,
                   priv->devices[i].name,
                   priv->devices[i].capabilities);
    }

#ifdef _WIN32
    // Initialize the multi-context device array
    memset(priv->device_contexts, 0, sizeof(priv->device_contexts));
    
    // Set up the first device in our array
    ALCdevice *dev = alcGetContextsDevice(priv->out_ctx);
    if (dev) {
        priv->device_contexts[0].device = dev;
        priv->device_contexts[0].context = priv->out_ctx;
        priv->device_contexts[0].source = priv->source;
        priv->device_contexts[0].active = 1;
        priv->active_device_index = 0;
    }
#endif

    return 0;
}

#ifdef _WIN32
static int force_audio_restart(struct iaxc_audio_driver *d, int output) {
    struct openal_priv_data* priv = d->priv;
    
    OPENAL_LOG("WINDOWS WORKAROUND: Switching to device %d (%s)", 
              output, priv->playDevices[output]);
    
    // Range check
    if (output < 0 || output >= priv->numPlay) {
        OPENAL_LOG("ERROR: Invalid device index %d", output);
        return -1;
    }
    
    // Check if we already have a context for this device
    if (priv->device_contexts[output].device == NULL) {
        // Need to create a new context for this device
        OPENAL_LOG("Creating new context for device: %s", priv->playDevices[output]);
        
        // Open the device
        ALCdevice *dev = alcOpenDevice(priv->playDevices[output]);
        if (!dev) {
            OPENAL_LOG("ERROR: Failed to open device");
            return -1;
        }
        
        // Create context
        ALCcontext *ctx = alcCreateContext(dev, NULL);
        if (!ctx) {
            OPENAL_LOG("ERROR: Failed to create context");
            alcCloseDevice(dev);
            return -1;
        }
        
        // Store in our array
        priv->device_contexts[output].device = dev;
        priv->device_contexts[output].context = ctx;
        
        // Activate context to set it up
        alcMakeContextCurrent(ctx);
        
        // Generate source
        alGenSources(1, &priv->device_contexts[output].source);
        
        // Configure source
        alSourcef(priv->device_contexts[output].source, AL_GAIN, priv->output_level);
        
        priv->device_contexts[output].active = 1;
    }
    
    // Switch to this device's context
    alcMakeContextCurrent(priv->device_contexts[output].context);
    
    // Update our state
    priv->selectedPlay = output;
    priv->active_device_index = output;
    
    // Set current context reference
    priv->out_ctx = priv->device_contexts[output].context;
    
    // Set the current source (for other functions to use)
    priv->source = priv->device_contexts[output].source;
    
    OPENAL_LOG("Now using device %d: %s", output, priv->playDevices[output]);
    
    // Trace device info
    ALCdevice *verifyDev = alcGetContextsDevice(alcGetCurrentContext());
    const ALCchar *verifyName = NULL;
    if (alcIsExtensionPresent(verifyDev, "ALC_ENUMERATE_ALL_EXT")) {
        verifyName = alcGetString(verifyDev, ALC_ALL_DEVICES_SPECIFIER);
        OPENAL_LOG("VERIFY: Now using device: %s", verifyName ? verifyName : "unknown");
    }
    
    return 0;
}
#endif

// Test microphone function - call this from your main app when debugging
int openal_test_microphone(struct iaxc_audio_driver *d, int seconds) {
    struct openal_priv_data* priv = d->priv;
    if (!priv->in_dev) {
        OPENAL_LOG("ERROR: No capture device to test");
        return -1;
    }
    
    OPENAL_LOG("Testing microphone for %d seconds...", seconds);
    
    // Start capture
    alcCaptureStart(priv->in_dev);
    
    // Sample buffer
    short samples[2000];  // Large enough for typical frame size
    int max_level = 0;
    int frame_count = 0;
    
    // Capture for specified seconds
    int total_ms = seconds * 1000;
    for (int ms = 0; ms < total_ms; ms += 50) {
        ALCint available;
        alcGetIntegerv(priv->in_dev, ALC_CAPTURE_SAMPLES, sizeof(available), &available);
        
        if (available > 0) {
            // Get at most 2000 samples
            int to_read = (available > 2000) ? 2000 : available;
            
            // Read samples
            alcCaptureSamples(priv->in_dev, samples, to_read);
            frame_count++;
            
            // Analyze levels
            for (int i = 0; i < to_read; i++) {
                int abs_val = abs(samples[i]);
                if (abs_val > max_level) max_level = abs_val;
            }
            
            // Log every 10 frames
            if (frame_count % 10 == 0) {
                OPENAL_LOG("Frame %d: %d samples, max level: %d (%d%%)", 
                          frame_count, to_read, max_level, (max_level * 100) / 32767);
            }
        }
        
        // Sleep a bit
        iaxc_millisleep(50);
    }
    
    OPENAL_LOG("Mic test complete - detected max level: %d (%d%%)", 
              max_level, (max_level * 100) / 32767);
              
    if (max_level < 500) {
        OPENAL_LOG("WARNING: Very low audio levels detected. Check microphone.");
    }
    
    return max_level > 0 ? 0 : -1;
}

