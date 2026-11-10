#include "runtime.h"
#include "bongo_cat/audio.h"

bool bongo_cat_app_step_live2d(BongoCatApp *app, float elapsed_seconds) {
    if (app && app->window_snapshot) return false;
    if (!app || !app->live2d || !app->loaded_model[0] || elapsed_seconds <= 0.0f) return false;
    bongo_cat_live2d_set_microphone_level(app->live2d,
        bongo_cat_audio_microphone_voice_level(app->audio));
    if (elapsed_seconds > 0.25f) elapsed_seconds = 0.25f;
    unsigned steps = 1;
    while (steps < 8 && elapsed_seconds / steps > 1.0f / 30.0f) steps++;
    float step = elapsed_seconds / steps;
    bool changed = false;
    for (unsigned i = 0; i < steps; ++i)
        changed = bongo_cat_live2d_update(app->live2d, step) || changed;
    if (changed) app->dirty = true;
    return changed;
}
