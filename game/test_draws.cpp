#include "test_draws.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
namespace uc {
namespace {
std::mutex lock;
std::vector<TestDrawStat> latest;
}
std::vector<TestDrawStat> summarize_draws(const gx::FramePacket& packet) {
    std::vector<TestDrawStat> out;
    for (const auto& draw : packet.draws) {
        const auto& t = draw.transforms;
        const bool indexed = draw.state.descriptors[0] != gx::Mode::none;   // same rule as the renderer's gpu_vertex call
        bool any = false; float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto& vertex : draw.vertices) {
            const uint32_t row = indexed ? vertex.position_matrix : t.current;
            if (row > 61) continue;
            const auto& p = vertex.position;
            float view[3];
            for (int i = 0; i < 3; ++i) { const auto& m = t.position_rows[row + i]; view[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]; }
            float clip[4];
            for (int i = 0; i < 4; ++i) { const auto& m = t.projection[i]; clip[i] = m[0] * view[0] + m[1] * view[1] + m[2] * view[2] + m[3]; }
            if (clip[3] <= 0) continue;   // behind the camera
            const float x = clip[0] / clip[3], y = -clip[1] / clip[3];
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); any = true;
        }
        if (!any) continue;
        TestDrawStat stat{0, draw.callsite, uint32_t(draw.vertices.size())};
        if (draw.texture && !draw.texture->levels.empty()) { stat.tex_w = draw.texture->levels[0].width; stat.tex_h = draw.texture->levels[0].height; stat.tex_id = uint64_t(reinterpret_cast<uintptr_t>(draw.texture.get())); }
        stat.x0 = x0; stat.y0 = y0; stat.x1 = x1; stat.y1 = y1;
        out.push_back(stat);
    }
    return out;
}
std::array<float,16> scene_projection(const gx::FramePacket& packet) {
    for (const auto& draw : packet.draws) {
        const auto& p = draw.transforms.projection;
        if (p[3][0] == 0 && p[3][1] == 0 && p[3][2] == -1 && p[3][3] == 0) {
            std::array<float,16> out{};
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = p[r][c];
            return out;
        }
    }
    return {};
}
std::string projection_json(const std::array<float,16>& m) {
    if (m == std::array<float,16>{}) return {};
    std::string out = "[";
    for (size_t i = 0; i < m.size(); ++i) { char number[32]; std::snprintf(number, sizeof number, "%s%.9g", i ? "," : "", m[i]); out += number; }
    return out + "]";
}
std::string test_draws_json(const std::vector<TestDrawStat>& stats) {
    std::string out = "[";
    for (size_t i = 0; i < stats.size(); ++i) {
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, R"(%s{"pass":%u,"callsite":%u,"vertices":%u,"tex":[%u,%u,%llu],"bbox":[%.4f,%.4f,%.4f,%.4f]})", i ? "," : "",
                      stats[i].pass, stats[i].callsite, stats[i].vertices, stats[i].tex_w, stats[i].tex_h, (unsigned long long)stats[i].tex_id, stats[i].x0, stats[i].y0, stats[i].x1, stats[i].y1);
        out += buffer;
    }
    return out + "]";
}
void test_draws_publish(std::vector<TestDrawStat> stats) { std::lock_guard g(lock); latest = std::move(stats); }
std::vector<TestDrawStat> test_draws_latest() { std::lock_guard g(lock); return latest; }
}
