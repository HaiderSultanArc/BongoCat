#include "audio_internal.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* App-thread owned. Streaming bounds decoded memory independently of clip
   duration; the pool bounds decoder/file handles during rapid key presses. */
void bongo_cat_audio_voice_release(AudioVoice *voice) {
    if (!voice->ready) return;
    ma_sound_stop(&voice->sound);
    /* uninit waits for pending decoding jobs before releasing their data. */
    ma_sound_uninit(&voice->sound);
    memset(voice, 0, sizeof(*voice));
}

BongoCatAudio *bongo_cat_audio_create(BongoCatError *error) {
    BongoCatAudio *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Cannot allocate audio player");
        return NULL;
    }
    audio->enabled = true;
    atomic_init(&audio->microphone_level, 0.0f);
    atomic_init(&audio->microphone_voice_level, 0.0f);
    return audio;
}

void bongo_cat_audio_stop(BongoCatAudio *audio) {
    if (!audio) return;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) bongo_cat_audio_voice_release(&audio->voices[i]);
#ifdef _WIN32
    SDL_SetAtomicInt(&audio->mixing, 0);
#endif
}

void bongo_cat_audio_reset(BongoCatAudio *audio) {
    if (!audio) return;
    bongo_cat_audio_stop(audio);
    if (audio->initialized) ma_engine_uninit(&audio->engine);
    audio->initialized = false;
    audio->next_collect = 0;
}

void bongo_cat_audio_collect(BongoCatAudio *audio, uint64_t now) {
    if (!audio || !audio->initialized || now < audio->next_collect) return;
    audio->next_collect = now + 1000;
    bool active = false;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) {
        AudioVoice *voice = &audio->voices[i];
        if (!voice->ready) continue;
        ma_result result = ma_resource_manager_data_source_result(
            voice->sound.pResourceManagerDataSource);
        if (result != MA_SUCCESS && result != MA_BUSY) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Cannot decode audio (%d): %s", result, voice->path);
            bongo_cat_audio_voice_release(voice);
        } else if (ma_sound_at_end(&voice->sound) || !ma_sound_is_playing(&voice->sound)) {
            if (now - voice->used >= AUDIO_IDLE_MS) bongo_cat_audio_voice_release(voice);
        } else active = true;
    }
#ifdef _WIN32
    if (ma_engine_get_device(&audio->engine)) {
        bool retained = false;
        for (size_t i = 0; i < AUDIO_VOICES; ++i)
            retained |= audio->voices[i].ready;
        if (!retained) SDL_SetAtomicInt(&audio->mixing, 0);
        return;
    }
#endif
    if (!active && now - audio->last_play >= AUDIO_IDLE_MS) bongo_cat_audio_reset(audio);
}

void bongo_cat_audio_update(BongoCatAudio *audio) {
    uint64_t now = SDL_GetTicks();
#ifdef _WIN32
    if (audio && audio->enabled && !audio->initialized && now >= audio->next_collect) {
        audio->next_collect = now + AUDIO_IDLE_MS;
        ma_result result = bongo_cat_audio_initialize(audio);
        if (result != MA_SUCCESS)
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Audio initialization failed: %d", result);
    }
#endif
    bongo_cat_audio_collect(audio, now);
}

void bongo_cat_audio_set_enabled(BongoCatAudio *audio, bool enabled) {
    if (!audio) return;
    audio->enabled = enabled;
    if (!enabled) bongo_cat_audio_reset(audio);
}

/* Voiced vowels have repeating waveforms. Require agreement in both shape and
   energy across a plausible pitch period to reject a single loud transient. */
static float microphone_voiced_rms(const float *samples) {
    enum {
        MIN_LAG = MICROPHONE_SAMPLE_RATE / 500,
        MAX_LAG = MICROPHONE_SAMPLE_RATE / 75 + 1,
        COMPARE = MICROPHONE_WINDOW - MAX_LAG,
    };
    const float *recent = samples + MAX_LAG;
    float energy = 0.0f;
    for (size_t i = 0; i < COMPARE; ++i) energy += recent[i] * recent[i];
    const float minimum_rms = 0.004f;
    if (energy < COMPARE * minimum_rms * minimum_rms) return 0.0f;

    float previous = 0.0f;
    bool rising = false;
    for (size_t lag = MIN_LAG - 1; lag <= MAX_LAG; ++lag) {
        const float *earlier = recent - lag;
        float dot = 0.0f, earlier_energy = 0.0f;
        for (size_t i = 0; i < COMPARE; ++i) {
            dot += recent[i] * earlier[i];
            earlier_energy += earlier[i] * earlier[i];
        }
        float similarity = 2.0f * dot / (energy + earlier_energy);
        // Look for a peak inside the pitch range rather than low rumble's slope.
        if (lag > MIN_LAG && rising && previous >= 0.74f && similarity < previous)
            return sqrtf(energy / COMPARE);
        rising = similarity > previous;
        previous = similarity;
    }
    return 0.0f;
}

void bongo_cat_audio_analyze_microphone(BongoCatAudio *audio, const float *samples, size_t count) {
    enum { HOP = MICROPHONE_SAMPLE_RATE / 100 };
    MicrophoneVoice *voice = &audio->microphone_voice;
    float energy = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        float sample = samples && isfinite(samples[i]) ? samples[i] : 0.0f;
        energy += sample * sample;
        // Remove low rumble and emphasize the range containing voice harmonics.
        voice->low += 0.06f * (sample - voice->low);
        voice->band += 0.50f * (sample - voice->low - voice->band);
        voice->samples[voice->count++] = voice->band;
        if (voice->count != MICROPHONE_WINDOW) continue;

        float level = microphone_voiced_rms(voice->samples);
        bool voiced = level > 0.0f;
        // Confirm onset across two windows and bridge one brief dropout.
        if (voiced == voice->previous_voiced)
            atomic_store_explicit(&audio->microphone_voice_level, level, memory_order_relaxed);
        voice->previous_voiced = voiced;
        memmove(voice->samples, voice->samples + HOP,
            (MICROPHONE_WINDOW - HOP) * sizeof(*voice->samples));
        voice->count = MICROPHONE_WINDOW - HOP;
    }
    float level = count ? fminf(1.0f, sqrtf(energy / (float)count)) : 0.0f;
    atomic_store_explicit(&audio->microphone_level, level, memory_order_relaxed);
}

static void capture_microphone(ma_device *device, void *output,
    const void *input, ma_uint32 frame_count) {
    (void)output;
    bongo_cat_audio_analyze_microphone(device->pUserData, input, frame_count);
}

void bongo_cat_audio_set_microphone_capture(BongoCatAudio *audio, bool enabled) {
    if (!audio) return;
    if (audio->microphone_initialized) {
        if (enabled && ma_device_is_started(&audio->microphone)) return;
        ma_device_uninit(&audio->microphone);
        audio->microphone_initialized = false;
    }
    atomic_store_explicit(&audio->microphone_level, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&audio->microphone_voice_level, 0.0f, memory_order_relaxed);
    audio->microphone_voice = (MicrophoneVoice){0};
    if (!enabled) return;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = MICROPHONE_SAMPLE_RATE;
    config.periodSizeInMilliseconds = 10;
    config.dataCallback = capture_microphone;
    config.pUserData = audio;
    config.wasapi.noAutoStreamRouting = MA_TRUE;
    if (ma_device_init(NULL, &config, &audio->microphone) != MA_SUCCESS) return;
    if (audio->microphone.pContext->backend != ma_backend_null &&
        ma_device_start(&audio->microphone) == MA_SUCCESS)
        audio->microphone_initialized = true;
    else
        ma_device_uninit(&audio->microphone);
}

float bongo_cat_audio_microphone_level(const BongoCatAudio *audio) {
    if (!audio || !audio->microphone_initialized ||
        !ma_device_is_started(&audio->microphone)) return -1.0f;
    return atomic_load_explicit(&audio->microphone_level, memory_order_relaxed);
}

float bongo_cat_audio_microphone_voice_level(const BongoCatAudio *audio) {
    if (!audio || !audio->microphone_initialized ||
        !ma_device_is_started(&audio->microphone)) return 0.0f;
    return atomic_load_explicit(&audio->microphone_voice_level, memory_order_relaxed);
}

void bongo_cat_audio_destroy(BongoCatAudio *audio) {
    if (!audio) return;
    bongo_cat_audio_set_microphone_capture(audio, false);
    bongo_cat_audio_reset(audio);
    free(audio);
}

bool bongo_cat_audio_is_playing(const BongoCatAudio *audio, const char *path) {
    if (!audio || !path || !path[0]) return false;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) {
        const AudioVoice *voice = &audio->voices[i];
        if (!voice->ready || strcmp(voice->path, path)) continue;
        ma_result result = ma_resource_manager_data_source_result(voice->sound.pResourceManagerDataSource);
        if ((result == MA_SUCCESS || result == MA_BUSY) &&
            ma_sound_is_playing(&voice->sound) && !ma_sound_at_end(&voice->sound)) return true;
    }
    return false;
}

void bongo_cat_audio_stop_path(BongoCatAudio *audio, const char *path) {
    if (!audio || !path || !path[0]) return;
    for (size_t i = 0; i < AUDIO_VOICES; ++i)
        if (audio->voices[i].ready && !strcmp(audio->voices[i].path, path))
            bongo_cat_audio_voice_release(&audio->voices[i]);
}

bool bongo_cat_audio_any_playing(const BongoCatAudio *audio) {
    if (!audio) return false;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) {
        const AudioVoice *voice = &audio->voices[i];
        if (!voice->ready) continue;
        ma_result result = ma_resource_manager_data_source_result(voice->sound.pResourceManagerDataSource);
        if ((result == MA_SUCCESS || result == MA_BUSY) &&
            ma_sound_is_playing(&voice->sound) && !ma_sound_at_end(&voice->sound)) return true;
    }
    return false;
}
