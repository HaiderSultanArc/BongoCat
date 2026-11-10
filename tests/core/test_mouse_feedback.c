#include "bongo_cat/app.h"
#include "test.h"

#include <stdio.h>

static bool mouse_back_light, mouse_forward_light;
static int scroll_pulses;

void bongo_cat_live2d_set_side_mouse_buttons(BongoCatLive2D *live2d, bool back, bool forward) {
    (void)live2d;
    mouse_back_light = back;
    mouse_forward_light = forward;
}

bool bongo_cat_live2d_pulse_scroll_wheel(BongoCatLive2D *live2d) {
    if (!live2d) return false;
    scroll_pulses++;
    return true;
}

static BongoCatInputEvent input(BongoCatInputKind kind, const char *name, float value) {
    BongoCatInputEvent event = {.kind = kind, .value = value};
    snprintf(event.name, sizeof(event.name), "%s", name);
    return event;
}

void bongo_cat_test_mouse_feedback(BongoCatApp *app) {
    BongoCatInputEvent event = input(BONGO_CAT_INPUT_MOUSE_DOWN, "Middle", 1.0f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(app->middle_mouse_down);
    event = input(BONGO_CAT_INPUT_MOUSE_UP, "Middle", 0.0f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(!app->middle_mouse_down);
    event = input(BONGO_CAT_INPUT_MOUSE_WHEEL, "Horizontal", -0.5f);
    bongo_cat_app_apply_input(app, &event);
    event = input(BONGO_CAT_INPUT_MOUSE_WHEEL, "Vertical", 1.25f);
    bongo_cat_app_apply_input(app, &event);
    event = input(BONGO_CAT_INPUT_MOUSE_WHEEL, "Vertical", -0.25f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(app->mouse_wheel_x == -0.5f && app->mouse_wheel_y == 1.0f);
    CHECK(scroll_pulses == 3 && app->dirty);
    app->dirty = false;
    event = input(BONGO_CAT_INPUT_MOUSE_WHEEL, "Vertical", 0.0f);
    bongo_cat_app_apply_input(app, &event);
    event = input(BONGO_CAT_INPUT_MOUSE_WHEEL, "Unknown", 1.0f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(scroll_pulses == 3 && !app->dirty);
    event = input(BONGO_CAT_INPUT_MOUSE_DOWN, "Back", 1.0f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(mouse_back_light && !mouse_forward_light);
    event = input(BONGO_CAT_INPUT_MOUSE_DOWN, "Forward", 1.0f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(app->side_mouse_down);
    CHECK(mouse_back_light && mouse_forward_light);
    event = input(BONGO_CAT_INPUT_MOUSE_UP, "Back", 0.0f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(app->side_mouse_down && app->forward_mouse_down);
    CHECK(!mouse_back_light && mouse_forward_light);
    event = input(BONGO_CAT_INPUT_MOUSE_UP, "Forward", 0.0f);
    bongo_cat_app_apply_input(app, &event);
    CHECK(!app->side_mouse_down);
    CHECK(!mouse_back_light && !mouse_forward_light);
}
