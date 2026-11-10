#ifndef BONGO_CAT_AUDIO_INTERNAL_H
#define BONGO_CAT_AUDIO_INTERNAL_H

#include "bongo_cat/audio.h"
#include <miniaudio.h>
#ifdef _WIN32
#include <SDL3/SDL_atomic.h>
#endif
#include <stdatomic.h>

#define AUDIO_VOICES 50
#define AUDIO_IDLE_MS 5000
#define MICROPHONE_SAMPLE_RATE 8000
#define MICROPHONE_WINDOW 240

typedef struct MicrophoneVoice {
    float samples[MICROPHONE_WINDOW];
    size_t count;
    float low;
    float band;
    bool previous_voiced;
} MicrophoneVoice;

typedef struct AudioVoice {
    ma_sound sound;
    char path[BONGO_CAT_PATH_CAP];
    uint64_t used;
    uint64_t order;
    bool ready;
} AudioVoice;

struct BongoCatAudio {
    ma_engine engine;
#ifdef _WIN32
    SDL_AtomicInt mixing;
#endif
    AudioVoice voices[AUDIO_VOICES];
    uint64_t sequence;
    uint64_t last_play;
    uint64_t next_collect;
    bool initialized;
    bool enabled;
    ma_device microphone;
    bool microphone_initialized;
    _Atomic float microphone_level;
    _Atomic float microphone_voice_level;
    MicrophoneVoice microphone_voice;
};

void bongo_cat_audio_analyze_microphone(BongoCatAudio *audio, const float *samples, size_t count);
void bongo_cat_audio_voice_release(AudioVoice *voice);
ma_result bongo_cat_audio_initialize(BongoCatAudio *audio);

/* Explicit time keeps idle reclamation deterministic in offline tests. */
void bongo_cat_audio_collect(BongoCatAudio *audio, uint64_t now_ms);

#endif
