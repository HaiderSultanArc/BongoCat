#include "cubism_model.hpp"
#include "cubism_model_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bongo_cat {
namespace {

constexpr int atlas_size = 2048;
constexpr float tap_hold = 0.130f;
constexpr float release_fade = 0.070f;
constexpr int patch_extent = 96;

struct Anchor { int x, y; };
struct AtlasPatch {
    int x, y;
    Anchor lights[mouse_light_count];
    bool rotated;
};

// The authored primary-click poses use rotated copies of the same mouse.
// The rectangles are aligned to eight texels so four mip levels can be
// updated without changing any pixels outside the corresponding atlas patch.
constexpr AtlasPatch patches[] = {
    {72, 176, {{99, 233}, {135, 245}, {123, 205}}, true},
    {800, 176, {{829, 233}, {865, 245}, {853, 205}}, true},
    {176, 1552, {{233, 1617}, {245, 1581}, {205, 1593}}, false},
};
struct LightStyle {
    float axis_x, axis_y, half_length, radius, glow_radius;
    float color[3];
};
constexpr LightStyle styles[mouse_light_count] = {
    {-0.42f, 0.9075f, 6.0f, 10.5f, 18.0f, {255.0f, 191.0f, 86.0f}},
    {-0.18f, 0.984f, 6.0f, 10.5f, 18.0f, {108.0f, 235.0f, 255.0f}},
    {0.22f, 0.976f, 7.0f, 8.0f, 14.0f, {228.0f, 146.0f, 255.0f}},
};

int brightness(float remaining) {
    return (int)std::lround(std::clamp(remaining / release_fade, 0.0f, 1.0f) * 255.0f);
}

float pill_distance(float x, float y, const LightStyle &style) {
    float along = x * style.axis_x + y * style.axis_y;
    float across = x * style.axis_y - y * style.axis_x;
    float end = std::max(0.0f, std::fabs(along) - style.half_length);
    return std::sqrt(across * across + end * end);
}

void tint(unsigned char *pixel, const float (&rgb)[3], float opacity) {
    for (int channel = 0; channel < 3; ++channel) {
        float target = rgb[channel] * (pixel[3] / 255.0f);
        pixel[channel] = (unsigned char)std::lround(
            pixel[channel] + (target - pixel[channel]) * opacity);
    }
}

void paint_patch(unsigned char *pixels, const AtlasPatch &patch,
    const std::array<int, mouse_light_count> &levels) {
    for (int y = 0; y < patch_extent; ++y) for (int x = 0; x < patch_extent; ++x) {
        unsigned char *pixel = pixels + (y * patch_extent + x) * 4;
        if (!pixel[3]) continue;
        for (size_t i = 0; i < levels.size(); ++i) {
            if (levels[i] == 0) continue;
            const Anchor &anchor = patch.lights[i];
            const LightStyle &style = styles[i];
            float dx = patch.x + x + 0.5f - anchor.x;
            float dy = patch.y + y + 0.5f - anchor.y;
            float d = patch.rotated ? pill_distance(dy, -dx, style) : pill_distance(dx, dy, style);
            if (d >= style.glow_radius) continue;
            float lit = levels[i] / 255.0f;
            float glow = std::clamp((style.glow_radius - d) /
                (style.glow_radius - style.radius + 1.5f), 0.0f, 1.0f);
            tint(pixel, style.color, lit * glow * glow * 0.65f);
            float coverage = std::clamp(style.radius - d, 0.0f, 1.0f);
            if (i == wheel_light) {
                tint(pixel, style.color, lit * coverage);
                continue;
            }
            float rim = 1.0f - 0.20f * std::clamp((d - 7.0f) / 3.0f, 0.0f, 1.0f);
            float fill[3];
            for (int c = 0; c < 3; ++c) fill[c] = style.color[c] * rim;
            tint(pixel, fill, coverage * lit);
        }
    }
}

// Match the model loader's box filter for premultiplied atlas pixels.
void halve_patch(unsigned char *pixels, int width, int height) {
    for (int y = 0; y < height / 2; ++y) for (int x = 0; x < width / 2; ++x) {
        const unsigned char *source = pixels + (2 * y * width + 2 * x) * 4;
        unsigned char *target = pixels + (y * (width / 2) + x) * 4;
        for (int c = 0; c < 4; ++c)
            target[c] = (unsigned char)((source[c] + source[4 + c] +
                source[width * 4 + c] + source[(width + 1) * 4 + c] + 2) / 4);
    }
}

} // namespace

void NativeModel::load_mouse_lights() {
    if (!has_hs_avatar()) return;

    GLint previous, width, height, filter;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, textures_[0]->id);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    if (width == atlas_size && height == atlas_size) {
        std::vector<unsigned char> atlas(atlas_size * atlas_size * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
        if (glGetError() == GL_NO_ERROR) {
            for (size_t i = 0; i < mouse_light_patches_.size(); ++i) {
                const AtlasPatch &patch = patches[i];
                auto &base = mouse_light_patches_[i];
                base.resize(patch_extent * patch_extent * 4);
                for (int y = 0; y < patch_extent; ++y)
                    std::memcpy(base.data() + y * patch_extent * 4,
                        atlas.data() + ((patch.y + y) * atlas_size + patch.x) * 4,
                        patch_extent * 4);
            }
            mouse_light_mip_levels_ = filter == GL_LINEAR_MIPMAP_LINEAR ? 4 : 1;
            for (auto &light : mouse_lights_) light.uploaded = -1;
        }
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
}

void NativeModel::set_side_mouse_buttons(bool back, bool forward) {
    if (!mouse_light_mip_levels_) return;
    // This mouse reports Back for the physical front button.
    const bool down[] = {forward, back};
    for (size_t i = rear_light; i <= front_light; ++i) {
        auto &light = mouse_lights_[i];
        if (down[i] && !light.down) {
            light.remaining = tap_hold + release_fade;
            light.pending = true;
        }
        light.down = down[i];
    }
}

bool NativeModel::pulse_scroll_wheel() {
    if (!mouse_light_mip_levels_) return false;
    auto &light = mouse_lights_[wheel_light];
    light.remaining = tap_hold + release_fade;
    light.pending = light.uploaded != 255;
    return light.pending;
}

bool NativeModel::update_mouse_lights(float delta_seconds) {
    if (!mouse_light_mip_levels_) return false;
    bool changed = false;
    for (auto &light : mouse_lights_) {
        if (!light.pending) light.remaining = std::max(light.down ? release_fade : 0.0f,
            light.remaining - delta_seconds);
        changed = changed || light.uploaded != brightness(light.remaining);
    }
    return changed;
}

void NativeModel::draw_mouse_lights() {
    if (!mouse_light_mip_levels_) return;
    std::array<int, mouse_light_count> levels{};
    bool changed = false;
    for (size_t i = 0; i < levels.size(); ++i) {
        auto &light = mouse_lights_[i];
        levels[i] = brightness(light.remaining);
        changed = changed || levels[i] != light.uploaded;
        light.pending = false;
        light.uploaded = levels[i];
    }
    if (!changed) return;

    GLint previous;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, textures_[0]->id);
    std::array<unsigned char, patch_extent * patch_extent * 4> pixels;
    for (size_t i = 0; i < mouse_light_patches_.size(); ++i) {
        const AtlasPatch &patch = patches[i];
        const auto &base = mouse_light_patches_[i];
        std::copy(base.begin(), base.end(), pixels.begin());
        paint_patch(pixels.data(), patch, levels);
        for (int level = 0; level < mouse_light_mip_levels_; ++level) {
            int extent = patch_extent >> level;
            glTexSubImage2D(GL_TEXTURE_2D, level, patch.x >> level, patch.y >> level,
                extent, extent, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            if (level + 1 < mouse_light_mip_levels_) halve_patch(pixels.data(), extent, extent);
        }
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
}

} // namespace bongo_cat
