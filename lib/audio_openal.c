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
  #include <windows.h>
  #define OPENAL_LOG(fmt, ...)                                                    \
    do {                                                                           \
      char _buf[512];                                                              \
      _snprintf(_buf, sizeof(_buf), "[openal-debug] " fmt "\n", ##__VA_ARGS__);   \
      OutputDebugStringA(_buf);                                                    \
    } while(0)
#else
  #define OPENAL_LOG(fmt, ...) fprintf(stderr, "[openal-debug] " fmt "\n", ##__VA_ARGS__)
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
};

/* single dummy entry (overwritten in initialize) */
static struct iaxc_audio_device device = {
    "default",
    IAXC_AD_INPUT  | IAXC_AD_INPUT_DEFAULT |
    IAXC_AD_OUTPUT | IAXC_AD_OUTPUT_DEFAULT |
    IAXC_AD_RING   | IAXC_AD_RING_DEFAULT,
    0
};

static int openal_error(const char* fn, int err) {
    fprintf(stderr, "[openal] %s failed (0x%X)\n", fn, err);
    return -1;
}

int openal_input(struct iaxc_audio_driver *d, void *samples, int *nSamples) {
    struct openal_priv_data* priv = d->priv;
    ALCint available;
    alcGetIntegerv(priv->in_dev, ALC_CAPTURE_SAMPLES, sizeof(available), &available);

    int req = (available < *nSamples) ? 0 : *nSamples;
    if (req > 0) {
        alcCaptureSamples(priv->in_dev, samples, req);
        if (alcGetError(priv->in_dev) != ALC_NO_ERROR) {
            openal_error("alcCaptureSamples", alcGetError(priv->in_dev));
            *nSamples = 0;
            return 1;
        }
        if (priv->input_level == 0.0f) {
            memset(samples, 0, req * sizeof(short));
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
    alcMakeContextCurrent(priv->out_ctx);
    openal_unqueue(priv);

    while (priv->buffers_free == 0) {
        iaxc_millisleep(10);
        openal_unqueue(priv);
    }

    ALuint buf = priv->buffers[priv->buffers_head++];
    if (priv->buffers_head >= priv->num_buffers)
        priv->buffers_head = 0;

    alBufferData(buf, AL_FORMAT_MONO16, samples, nSamples * sizeof(short), priv->sample_rate);
    alSourceQueueBuffers(priv->source, 1, &buf);
    priv->buffers_free--;

    if (priv->buffers_free == priv->num_buffers - 2) {
        ALint st;
        alGetSourcei(priv->source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING)
            alSourcePlay(priv->source);
    }

    return 0;
}

int openal_select_devices(struct iaxc_audio_driver *d, int input, int output, int ring) {
    struct openal_priv_data* priv = d->priv;
    alcMakeContextCurrent(priv->out_ctx);

    /* switch capture device */
    if (input >= 0 && input < priv->numCapture && input != priv->selectedCapture) {
        OPENAL_LOG("Switching capture device: %d -> %d", priv->selectedCapture, input);
        alcCaptureStop(priv->in_dev);
        alcCaptureCloseDevice(priv->in_dev);
        priv->selectedCapture = input;
        priv->in_dev = alcCaptureOpenDevice(
            priv->captureDevices[input],
            priv->sample_rate,
            AL_FORMAT_MONO16,
            priv->sample_rate / 2
        );
        alcCaptureStart(priv->in_dev);
    }

    /* switch playback device */
    if (output >= 0 && output < priv->numPlay && output != priv->selectedPlay) {
        ALCdevice *oldDev = alcGetContextsDevice(priv->out_ctx);
        OPENAL_LOG("Switching playback device: %d -> %d", priv->selectedPlay, output);
        alcMakeContextCurrent(NULL);
        alcDestroyContext(priv->out_ctx);
        alcCloseDevice(oldDev);

        priv->selectedPlay = output;
        ALCdevice *newDev = alcOpenDevice(priv->playDevices[output]);
        priv->out_ctx = alcCreateContext(newDev, NULL);
        alcMakeContextCurrent(priv->out_ctx);

        /* regenerate buffers/source */
        alDeleteSources(1, &priv->source);
        alGenSources(1, &priv->source);
        alGenBuffers(priv->num_buffers, priv->buffers);
        priv->buffers_free = priv->num_buffers;
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

int openal_start(struct iaxc_audio_driver *d) { (void)d; return 0; }
int openal_stop (struct iaxc_audio_driver *d) { (void)d; return 0; }

float openal_input_level_get(struct iaxc_audio_driver *d) {
    return ((struct openal_priv_data*)d->priv)->input_level;
}
float openal_output_level_get(struct iaxc_audio_driver *d) {
    return ((struct openal_priv_data*)d->priv)->output_level;
}
int openal_input_level_set(struct iaxc_audio_driver *d, float lvl) {
    ((struct openal_priv_data*)d->priv)->input_level = (lvl < 0.5f ? 0.f : 1.f);
    return 0;
}
int openal_output_level_set(struct iaxc_audio_driver *d, float lvl) {
    struct openal_priv_data* priv = d->priv;
    priv->output_level = lvl;
    alSourcef(priv->source, AL_GAIN, lvl);
    return 0;
}

int openal_play_sound(struct iaxc_sound *s, int ring) { (void)s; (void)ring; return 0; }
int openal_stop_sound(int id)                          { (void)id; return 0; }
int openal_mic_boost_get(struct iaxc_audio_driver *d)  { (void)d; return 0; }
int openal_mic_boost_set(struct iaxc_audio_driver *d, int e) { (void)d; (void)e; return 0; }

int openal_destroy(struct iaxc_audio_driver *d) {
    struct openal_priv_data* priv = d->priv;

    /* stop & close capture */
    alcCaptureStop(priv->in_dev);
    alcCaptureCloseDevice(priv->in_dev);

    /* delete source & buffers */
    alDeleteSources(1, &priv->source);
    alDeleteBuffers(priv->num_buffers, priv->buffers);

    /* tear down playback context */
    ALCdevice *playDev = alcGetContextsDevice(priv->out_ctx);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(priv->out_ctx);
    alcCloseDevice(playDev);

    /* free everything */
    free((void*)priv->captureDevices);
    free((void*)priv->playDevices);
    free(priv->buffers);
    free(priv->devices);
    free(priv);

    d->priv = NULL;
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
    priv->in_dev = alcCaptureOpenDevice(
        priv->captureDevices[0],
        sample_rate,
        AL_FORMAT_MONO16,
        sample_rate/2
    );
    OPENAL_LOG("Opening default capture device '%s' @ %dHz",
               priv->captureDevices[0], sample_rate);
    if (!priv->in_dev) return openal_error("alcCaptureOpenDevice", alcGetError(NULL));
    alcCaptureStart(priv->in_dev);

    /* prepare playback buffers */
    priv->num_buffers = 16;
    priv->buffers     = malloc(priv->num_buffers * sizeof(ALuint));
    alGenBuffers(priv->num_buffers, priv->buffers);
    alGenSources(1, &priv->source);
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

    return 0;
}
