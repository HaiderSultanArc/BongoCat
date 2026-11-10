#include "cubism_model.hpp"

#include <Id/CubismIdManager.hpp>
#include <algorithm>
#include <cmath>

namespace bongo_cat {
namespace {

struct Point { float x, y; };

bool texture_point(Csm::CubismModel &model, int mesh, Point uv, Point &position) {
    const auto *coordinates = model.GetDrawableVertexUvs(mesh);
    const auto *vertices = model.GetDrawableVertices(mesh);
    const auto *indices = model.GetDrawableVertexIndices(mesh);
    int vertex_count = model.GetDrawableVertexCount(mesh);
    int index_count = model.GetDrawableVertexIndexCount(mesh);
    for (int i = 0; coordinates && vertices && indices && i + 2 < index_count; i += 3) {
        int a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) continue;
        const auto &origin = coordinates[a];
        Point u{coordinates[b].X - origin.X, coordinates[b].Y - origin.Y};
        Point v{coordinates[c].X - origin.X, coordinates[c].Y - origin.Y};
        Point p{uv.x - origin.X, uv.y - origin.Y};
        float det = u.x * v.y - u.y * v.x;
        if (std::fabs(det) < 0.0000001f) continue;
        float s = (p.x * v.y - p.y * v.x) / det;
        float t = (u.x * p.y - u.y * p.x) / det;
        float w = 1.0f - s - t;
        if (s < 0.0f || t < 0.0f || w < 0.0f) continue;
        position = {w * vertices[2 * a] + s * vertices[2 * b] + t * vertices[2 * c],
            w * vertices[2 * a + 1] + s * vertices[2 * b + 1] + t * vertices[2 * c + 1]};
        return true;
    }
    return false;
}

// Mouth center, right edge and downward axis on the Dark Souls face atlas.
constexpr Point mouth_center{1279.5f / 2048.0f, 1.0f - 1275.5f / 2048.0f};
constexpr Point mouth_right{1303.5f / 2048.0f, 1.0f - 1273.5f / 2048.0f};
constexpr Point mouth_down{1281.5f / 2048.0f, 1.0f - 1299.5f / 2048.0f};

constexpr char vertex_source[] =
    "#version 330 core\n"
    "layout(location=0) in vec2 position; layout(location=1) in vec2 local;"
    "layout(location=2) in float opacity; out vec2 uv; out float alpha;"
    "void main(){gl_Position=vec4(position,0,1);uv=local;alpha=opacity;}";
constexpr char fragment_source[] =
    "#version 330 core\n"
    "in vec2 uv; in float alpha; out vec4 color;"
    "void main(){float d=length(uv);float edge=max(fwidth(d),.01);"
    "float a=alpha*(1.-smoothstep(1.-edge,1.+edge,d));"
    "vec3 inside=vec3(.15,.046,.04);"
    "float tongue=1.-smoothstep(.82,1.1,length((uv-vec2(0,.4))/vec2(.58,.35)));"
    "inside=mix(inside,vec3(.64,.25,.22),tongue);"
    "vec3 rgb=mix(inside,vec3(.23,.09,.055),smoothstep(.74,.94,d));"
    "color=vec4(rgb*a,a);}";

} // namespace

void NativeModel::load_microphone_mouth() {
    for (Csm::csmUint32 i = 0; i < lip_sync_ids_.GetSize(); ++i) {
        int index = _model->GetParameterIndex(lip_sync_ids_[i]);
        if (index >= 0 && index < _model->GetParameterCount())
            microphone_parameters_.push_back(index);
    }
    if (!microphone_parameters_.empty() || !has_hs_avatar()) return;

    // This skin's mouth is painted into the head texture, with no mouth keyforms.
    auto *ids = Csm::CubismFramework::GetIdManager();
    int mesh = _model->GetDrawableIndex(ids->GetId("ArtMesh6"));
    if (mesh < 0 || mesh >= _model->GetDrawableCount()) return;
    if (_model->GetDrawableTextureIndex(mesh) != 0) return;
    Point position{};
    if (texture_point(*_model, mesh, mouth_center, position) &&
        texture_point(*_model, mesh, mouth_right, position) &&
        texture_point(*_model, mesh, mouth_down, position))
        microphone_face_ = mesh;
}

void NativeModel::set_microphone_level(float rms) {
    constexpr float full_open = 0.085f;
    microphone_target_ = rms > 0.0f ? std::sqrt(std::min(1.0f, rms / full_open)) : 0.0f;
}

bool NativeModel::update_microphone_mouth(float delta_seconds) {
    if (microphone_face_ < 0 && microphone_parameters_.empty()) return false;
    float previous = microphone_open_;
    if (microphone_target_ > 0.0f) {
        float response = microphone_target_ > previous ? 0.024f : 0.040f;
        microphone_open_ += (microphone_target_ - previous) *
            (1.0f - std::exp(-delta_seconds / response));
    } else {
        microphone_open_ = std::max(0.0f, previous - delta_seconds / 0.040f);
    }

    if (microphone_open_ > 0.0f) {
        for (int index : microphone_parameters_) {
            float rest = _model->GetParameterDefaultValue(index);
            float open = rest + microphone_open_ *
                (_model->GetParameterMaximumValue(index) - rest);
            _model->SetParameterValue(index, std::max(open, _model->GetParameterValue(index)));
        }
    }
    return microphone_face_ >= 0 && previous != microphone_open_;
}

void NativeModel::draw_microphone_mouth(Csm::CubismMatrix44 &projection) {
    if (microphone_face_ < 0 || microphone_open_ <= 0.0f || microphone_gl_failed_ ||
        !_model->GetDrawableDynamicFlagIsVisible(microphone_face_)) return;
    Point center{}, right{}, down{};
    if (!texture_point(*_model, microphone_face_, mouth_center, center) ||
        !texture_point(*_model, microphone_face_, mouth_right, right) ||
        !texture_point(*_model, microphone_face_, mouth_down, down)) return;

    GLint program, vao, buffer, src_rgb, dst_rgb, src_alpha, dst_alpha;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
    bool blended = glIsEnabled(GL_BLEND), culled = glIsEnabled(GL_CULL_FACE);
    bool depth = glIsEnabled(GL_DEPTH_TEST);
    if (!microphone_program_) {
        BongoCatError error{};
        if (bongo_cat_gl_load(&microphone_gl_, &error))
            microphone_program_ = bongo_cat_gl_program(&microphone_gl_,
                vertex_source, fragment_source, &error);
        if (!microphone_program_) {
            microphone_gl_failed_ = true;
            return;
        }
        microphone_gl_.gen_vertex_arrays(1, &microphone_vao_);
        microphone_gl_.gen_buffers(1, &microphone_vbo_);
        microphone_gl_.bind_vertex_array(microphone_vao_);
        microphone_gl_.bind_buffer(GL_ARRAY_BUFFER, microphone_vbo_);
        for (GLuint i = 0; i < 3; ++i) {
            microphone_gl_.enable_attribute(i);
            microphone_gl_.attribute_pointer(i, i == 2 ? 1 : 2, GL_FLOAT, GL_FALSE,
                5 * sizeof(float), reinterpret_cast<const void *>(2 * i * sizeof(float)));
        }
    }

    Point x_axis{right.x - center.x, right.y - center.y};
    Point y_axis{down.x - center.x, down.y - center.y};
    float height = 0.13f + 0.61f * microphone_open_;
    float opacity = std::min(1.0f, microphone_open_ * 8.0f) * _opacity *
        _model->GetDrawableOpacity(microphone_face_);
    constexpr float corners[4][2] = {{-1,-1}, {1,-1}, {-1,1}, {1,1}};
    float vertices[4][5];
    for (int i = 0; i < 4; ++i) {
        float x = corners[i][0], y = corners[i][1];
        vertices[i][0] = projection.TransformX(center.x + x * x_axis.x +
            (y + 0.32f) * height * y_axis.x);
        vertices[i][1] = projection.TransformY(center.y + x * x_axis.y +
            (y + 0.32f) * height * y_axis.y);
        vertices[i][2] = x;
        vertices[i][3] = y;
        vertices[i][4] = opacity;
    }
    glEnable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    microphone_gl_.blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    microphone_gl_.use_program(microphone_program_);
    microphone_gl_.bind_vertex_array(microphone_vao_);
    microphone_gl_.bind_buffer(GL_ARRAY_BUFFER, microphone_vbo_);
    microphone_gl_.buffer_data(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    microphone_gl_.bind_vertex_array(vao);
    microphone_gl_.bind_buffer(GL_ARRAY_BUFFER, buffer);
    microphone_gl_.use_program(program);
    microphone_gl_.blend_func_separate(src_rgb, dst_rgb, src_alpha, dst_alpha);
    if (!blended) glDisable(GL_BLEND);
    if (culled) glEnable(GL_CULL_FACE);
    if (depth) glEnable(GL_DEPTH_TEST);
}

void NativeModel::release_microphone_mouth() {
    if (microphone_vbo_) microphone_gl_.delete_buffers(1, &microphone_vbo_);
    if (microphone_vao_) microphone_gl_.delete_vertex_arrays(1, &microphone_vao_);
    if (microphone_program_) microphone_gl_.delete_program(microphone_program_);
    microphone_program_ = microphone_vao_ = microphone_vbo_ = 0;
}

} // namespace bongo_cat
