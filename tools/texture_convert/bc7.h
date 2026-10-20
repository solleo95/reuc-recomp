#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace uc {
// BC7 mode 6 only (one subset, RGBA 7+1 bit endpoints, 4-bit indices): it carries alpha without a mode switch and is the quality/speed
// compromise for upscaled art. shortcut: no partition modes, so blocks with two unrelated colour ramps show banding; upgrade to modes 1/3/5 if seen.
namespace bc7 {
constexpr int kWeight[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
inline int mix(int a, int b, int w) { return (a * (64 - w) + b * w + 32) >> 6; }

struct Endpoint { int q[4]; int p; int value[4]; };
// Nearest representable endpoint (7-bit channels sharing one p-bit) to a target colour.
inline Endpoint quantize(const float target[4]) {
    Endpoint best{}; float best_error = 1e30f;
    for (int p = 0; p < 2; ++p) {
        Endpoint e{}; e.p = p; float error = 0;
        for (int c = 0; c < 4; ++c) {
            e.q[c] = std::clamp(int(std::lround((target[c] - p) / 2)), 0, 127);
            e.value[c] = (e.q[c] << 1) | p;
            error += (e.value[c] - target[c]) * (e.value[c] - target[c]);
        }
        if (error < best_error) { best_error = error; best = e; }
    }
    return best;
}
// Closest of the 16 ramp colours for every pixel; returns the summed squared error.
inline uint32_t assign(const uint8_t* px, const Endpoint& a, const Endpoint& b, uint8_t* index) {
    int ramp[16][4];
    for (int k = 0; k < 16; ++k) for (int c = 0; c < 4; ++c) ramp[k][c] = mix(a.value[c], b.value[c], kWeight[k]);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) {
        uint32_t best = ~0u; int pick = 0;
        for (int k = 0; k < 16; ++k) {
            uint32_t error = 0;
            for (int c = 0; c < 4; ++c) { const int d = ramp[k][c] - px[i * 4 + c]; error += uint32_t(d * d); }
            if (error < best) { best = error; pick = k; }
        }
        index[i] = uint8_t(pick); total += best;
    }
    return total;
}
struct Writer {
    uint8_t* out; int pos = 0;
    void put(uint32_t value, int bits) { for (int i = 0; i < bits; ++i, ++pos) if ((value >> i) & 1) out[pos >> 3] |= uint8_t(1 << (pos & 7)); }
};
} // namespace bc7

// `px`: 16 RGBA8 pixels, row-major. `out`: 16 bytes.
inline void bc7_encode_block(const uint8_t* px, uint8_t* out) {
    using namespace bc7;
    float mean[4] = {}, cov[4][4] = {};
    for (int i = 0; i < 16; ++i) for (int c = 0; c < 4; ++c) mean[c] += px[i * 4 + c] / 16.f;
    for (int i = 0; i < 16; ++i) for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) cov[a][b] += (px[i * 4 + a] - mean[a]) * (px[i * 4 + b] - mean[b]);
    float axis[4] = {1, 1, 1, 1};
    for (int step = 0; step < 4; ++step) {
        float next[4] = {}, length = 0;
        for (int a = 0; a < 4; ++a) { for (int b = 0; b < 4; ++b) next[a] += cov[a][b] * axis[b]; length += next[a] * next[a]; }
        length = std::sqrt(length);
        if (length < 1e-6f) { std::fill(axis, axis + 4, 0.f); break; }
        for (int a = 0; a < 4; ++a) axis[a] = next[a] / length;
    }
    float low = 0, high = 0;
    for (int i = 0; i < 16; ++i) {
        float t = 0; for (int c = 0; c < 4; ++c) t += (px[i * 4 + c] - mean[c]) * axis[c];
        low = std::min(low, t); high = std::max(high, t);
    }
    float target[2][4];
    for (int c = 0; c < 4; ++c) { target[0][c] = std::clamp(mean[c] + axis[c] * low, 0.f, 255.f); target[1][c] = std::clamp(mean[c] + axis[c] * high, 0.f, 255.f); }

    Endpoint best[2]{}; uint8_t best_index[16]; uint32_t best_error = ~0u;
    for (int round = 0; round < 4; ++round) {
        const Endpoint a = quantize(target[0]), b = quantize(target[1]);
        uint8_t index[16]; const uint32_t error = assign(px, a, b, index);
        if (error >= best_error) break;
        best_error = error; best[0] = a; best[1] = b; std::memcpy(best_index, index, 16);
        if (!error) break;
        // Least-squares refit of both ends for the chosen indices: x ~ (1-t) e0 + t e1.
        float saa = 0, sab = 0, sbb = 0, sax[4] = {}, sbx[4] = {};
        for (int i = 0; i < 16; ++i) {
            const float t = kWeight[index[i]] / 64.f, wa = 1 - t;
            saa += wa * wa; sab += wa * t; sbb += t * t;
            for (int c = 0; c < 4; ++c) { sax[c] += wa * px[i * 4 + c]; sbx[c] += t * px[i * 4 + c]; }
        }
        const float det = saa * sbb - sab * sab;
        if (std::fabs(det) < 1e-4f) break;
        for (int c = 0; c < 4; ++c) {
            target[0][c] = std::clamp((sax[c] * sbb - sbx[c] * sab) / det, 0.f, 255.f);
            target[1][c] = std::clamp((sbx[c] * saa - sax[c] * sab) / det, 0.f, 255.f);
        }
    }
    // The first index drops its top bit: keep it below 8 by swapping the endpoints (the ramp is symmetric).
    if (best_index[0] >= 8) { std::swap(best[0], best[1]); for (auto& i : best_index) i = uint8_t(15 - i); }
    std::memset(out, 0, 16);
    Writer w{out};
    w.put(64, 7);
    for (int c = 0; c < 4; ++c) { w.put(uint32_t(best[0].q[c]), 7); w.put(uint32_t(best[1].q[c]), 7); }
    w.put(uint32_t(best[0].p), 1); w.put(uint32_t(best[1].p), 1);
    for (int i = 0; i < 16; ++i) w.put(best_index[i], i ? 4 : 3);
}

// Mode 6 decoder for the self-test; returns false for any other mode.
inline bool bc7_decode_block(const uint8_t* in, uint8_t* rgba) {
    int pos = 0;
    auto get = [&](int bits) { uint32_t v = 0; for (int i = 0; i < bits; ++i, ++pos) v |= uint32_t((in[pos >> 3] >> (pos & 7)) & 1) << i; return v; };
    if (get(7) != 64) return false;
    int q[2][4];
    for (int c = 0; c < 4; ++c) { q[0][c] = int(get(7)); q[1][c] = int(get(7)); }
    const int p0 = int(get(1)), p1 = int(get(1));
    int value[2][4];
    for (int c = 0; c < 4; ++c) { value[0][c] = (q[0][c] << 1) | p0; value[1][c] = (q[1][c] << 1) | p1; }
    for (int i = 0; i < 16; ++i) {
        const int k = int(get(i ? 4 : 3));
        for (int c = 0; c < 4; ++c) rgba[i * 4 + c] = uint8_t(bc7::mix(value[0][c], value[1][c], bc7::kWeight[k]));
    }
    return true;
}
} // namespace uc
