#pragma once
#include "gx_vertices.h"
#include <array>
#include <cmath>
#include <optional>

namespace gx {
// The aim reticle is drawn by the game from the pointer it read at the start of its logic step, so it trails the mouse by up to a step plus the frames
// in flight. The renderer re-aims it at record time: the draws below are shifted by (freshest pointer - pointer the game drew with), which also keeps
// whatever the game adds itself (recoil, easing). Aim units: [-1,1] spans 1.25 screen widths and heights x 1.67 (aim = ndc * (0.8, 0.6)).
constexpr float kAimToNdcX = 1.f / 0.8f, kAimToNdcY = 1.f / 0.6f;
// Quads of the reticle ring and centre dot: (callsite, square texture edge). Found with the harness `draws` request at three aim points.
inline bool reticle_callsite(uint32_t callsite) { return callsite == 0x80393B0C || callsite == 0x8006D174; }
inline bool reticle_texture(const TexturePixels& texture) {
    if (texture.job && !texture.job->done.load(std::memory_order_acquire)) return false;   // not decoded yet: decide on a later frame
    if (texture.levels.empty()) return false;
    const auto& level = texture.levels[0];
    const uint32_t width = texture.guest_width ? texture.guest_width : level.width, height = texture.guest_height ? texture.guest_height : level.height;
    return width == height && (width == 100 || width == 120 || width == 154);
}
// Centre of the draw's screen-space bounding box in NDC (x right, y up), like the harness's draw summary.
inline std::optional<std::array<float,2>> draw_center_ndc(const DrawPacket& draw) {
    const auto& t = draw.transforms;
    const bool indexed = draw.state->descriptors[0] != Mode::none;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; bool any = false;
    for (const auto& vertex : draw.vertex_view()) {
        const uint32_t row = indexed ? vertex.position_matrix : t->current;
        if (row > 61) continue;
        const auto& p = vertex.position; float view[3];
        for (int i = 0; i < 3; ++i) { const auto& m = t->position_rows[row + i]; view[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]; }
        float clip[4];
        for (int i = 0; i < 4; ++i) { const auto& m = t->projection[i]; clip[i] = m[0] * view[0] + m[1] * view[1] + m[2] * view[2] + m[3]; }
        if (clip[3] <= 0) continue;
        const float x = clip[0] / clip[3], y = clip[1] / clip[3];
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); any = true;
    }
    if (!any) return std::nullopt;
    return std::array<float,2>{(x0 + x1) / 2, (y0 + y1) / 2};
}
// Is this draw a reticle quad of a frame whose game drew with `consumed` (aim units)?
inline bool is_reticle_draw(const DrawPacket& draw, const std::array<float,2>& consumed) {
    if (!reticle_callsite(draw.callsite) || !draw.texture || !reticle_texture(*draw.texture)) return false;
    const auto center = draw_center_ndc(draw);
    if (!center) return false;
    // It must sit near the pointer the game drew with (recoil moves it a little): a ring elsewhere on the screen is something else.
    return std::abs((*center)[0] - consumed[0] * kAimToNdcX) < 0.25f && std::abs((*center)[1] + consumed[1] * kAimToNdcY) < 0.25f;
}
}
