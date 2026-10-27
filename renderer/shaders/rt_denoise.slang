// The own denoiser of a ray-traced signal (renderer/rt_denoiser.h), SVGF-style: temporalMain accumulates the noisy signal into a history reprojected
// by the scene's motion vectors (rejected on depth and normal, renderer/rt_denoise_math.h), atrousMain is one pass of a 5x5 edge-aware à-trous
// filter (depth, normal and, for radiance, luminance weighted by the estimated variance), run with steps 1, 2, 4, ...
#include "gbuffer.slang"
struct Params {
    uint2 size;          // the signal's size in pixels
    uint2 region;        // the scene region it covers, in scene-target pixels
    float2 depth;        // view parameters depthA, depthB
    float2 depthRange;   // the viewport's near and far depth
    uint signal;         // rt_denoiser.h RtSignal: 2 = radiance
    uint flags;          // bit 0: no history (reset)
    uint step;           // à-trous: tap spacing in pixels
    uint first;          // à-trous: the first pass (its variance is in the moments, its input the history)
    uint maxHistory;     // temporal: the longest history in frames
};
#ifdef UC_DIRECTX
ConstantBuffer<Params> params : register(b0);
Texture2D<float4> noisy : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float2> sceneMotion : register(t2);
Texture2D<float4> sceneGBuffer : register(t3);
RWTexture2D<float4> previousHistory : register(u0);
RWTexture2D<float4> previousMoments : register(u1);
RWTexture2D<float4> previousGuide : register(u2);
RWTexture2D<float4> history : register(u3);
RWTexture2D<float4> moments : register(u4);
RWTexture2D<float4> guide : register(u5);
RWTexture2D<float4> filterIn : register(u6);
RWTexture2D<float4> filterOut : register(u7);
#else
[[vk::push_constant]] ConstantBuffer<Params> params;
[[vk::binding(0, 0)]] Texture2D<float4> noisy;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float2> sceneMotion;   // previous minus current position, scene-target pixels
[[vk::binding(3, 0)]] Texture2D<float4> sceneGBuffer;
// History: rgb = accumulated signal, a = history length in frames (0 = no surface). Moments: r, g = first and second moment of the luminance,
// b = its variance. Guide: r = view distance (0 = no surface), gba = view normal.
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> previousHistory;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> previousMoments;
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> previousGuide;
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> history;
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> moments;
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> guide;
// À-trous: rgb = signal, a = its variance.
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> filterIn;
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> filterOut;
#endif

static const uint kReset = 1, kRadiance = 2;
static const float kDepthTolerance = 0.02;        // kDenoiseDepthTolerance
static const float kNormalCos = 0.906307787;      // kDenoiseNormalCos (25 degrees)
// shortcut: a history shorter than 4 frames has no usable variance; a fixed large one keeps the luminance weight loose. SVGF estimates it spatially (upgrade if young radiance looks noisy).
static const float kYoungVariance = 1.0;
static const float kSigmaDepth = 1.0, kDepthFloor = 0.01, kSigmaNormal = 128.0, kSigmaLuminance = 4.0;

float historyAlpha(float length) { return max(1.0 / max(length, 1.0), 1.0 / float(params.maxHistory)); }   // rt_denoise_math.h history_alpha, kDenoiseMaxHistory = the default
bool reprojectValid(float distance, float historyDistance, float normalDot) { return abs(distance - historyDistance) <= kDepthTolerance * distance && normalDot >= kNormalCos; }
float luminance(float3 c) { return params.signal == kRadiance ? dot(c, float3(0.2126, 0.7152, 0.0722)) : c.r; }

// The scene surface under a signal pixel: its view distance (0 = none) and view normal.
float surfaceAt(int2 pixel, out float3 normal) {
    normal = float3(0.0, 0.0, 1.0);
    const int2 scenePixel = int2((float2(pixel) + 0.5) * float2(params.region) / float2(params.size));
    const float4 texel = sceneGBuffer.Load(int3(scenePixel, 0));
    if ((uint(round(texel.w * 65535.0)) & kGBufferHasNormal) == 0) return 0.0;
    const float stored = sceneDepth.Load(int3(scenePixel, 0));
    if (stored >= 1.0) return 0.0;
    normal = decodeGBufferNormal(texel.xy);
    const float projected = (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x);
    return params.depth.y / (projected + params.depth.x);   // -z of view_space.slang reconstructView
}

[numthreads(8, 8, 1)]
void temporalMain(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (any(p >= int2(params.size))) return;
    const float3 current = noisy.Load(int3(p, 0)).rgb;
    float3 normal;
    const float distance = surfaceAt(p, normal);
    guide[p] = float4(distance, normal);
    if (distance <= 0.0) { history[p] = float4(current, 0.0); moments[p] = float4(0.0, 0.0, 0.0, 0.0); return; }   // nothing to accumulate on
    const float l = luminance(current);
    float length = 1.0;
    float3 color = current;
    float2 m = float2(l, l * l);
    if ((params.flags & kReset) == 0) {
        // shortcut: the nearest history pixel; SVGF's bilinear 2x2 with a test per tap if moving surfaces look blocky
        const int2 scenePixel = int2((float2(p) + 0.5) * float2(params.region) / float2(params.size));
        const float2 motion = sceneMotion.Load(int3(scenePixel, 0)) * float2(params.size) / float2(params.region);
        const int2 q = int2(floor(float2(p) + 0.5 + motion));
        if (all(q >= int2(0, 0)) && all(q < int2(params.size))) {
            const float4 previous = previousHistory[q];
            const float4 previousSurface = previousGuide[q];
            if (previous.a > 0.0 && reprojectValid(distance, previousSurface.x, dot(normal, previousSurface.yzw))) {
                length = min(previous.a + 1.0, float(params.maxHistory));
                const float alpha = historyAlpha(length);
                color = lerp(previous.rgb, current, alpha);
                m = lerp(previousMoments[q].rg, m, alpha);
            }
        }
    }
    float variance = max(m.y - m.x * m.x, 0.0);
    if (length < 4.0) variance = max(variance, kYoungVariance);
    history[p] = float4(color, length);
    moments[p] = float4(m, variance, 0.0);
}

float varianceAt(int2 pixel, float4 value) { return params.first != 0 ? moments[pixel].b : value.a; }
// One side's depth change toward a neighbour: the smaller of the two, so a depth edge next to the pixel does not count as its slope.
float slope(int2 p, int2 axis, float z) {
    float best = 0.0; bool found = false;
    for (int s = -1; s <= 1; s += 2) {
        const int2 q = p + axis * s;
        if (any(q < int2(0, 0)) || any(q >= int2(params.size))) continue;
        const float zq = guide[q].x;
        if (zq <= 0.0) continue;
        const float d = (zq - z) * float(s);
        if (!found || abs(d) < abs(best)) { best = d; found = true; }
    }
    return best;
}

[numthreads(8, 8, 1)]
void atrousMain(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (any(p >= int2(params.size))) return;
    const float4 center = filterIn[p];
    const float centerVariance = varianceAt(p, center);
    const float4 surface = guide[p];
    if (surface.x <= 0.0) { filterOut[p] = float4(center.rgb, centerVariance); return; }
    const float z = surface.x;
    const float3 n = surface.yzw;
    const float2 gradient = float2(slope(p, int2(1, 0), z), slope(p, int2(0, 1), z));
    const float luminanceScale = params.signal == kRadiance ? 1.0 / (kSigmaLuminance * sqrt(centerVariance) + 1e-4) : 0.0;
    const float l = luminance(center.rgb);
    const float kernel[3] = { 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };
    float3 sum = 0.0; float weights = 0.0, variance = 0.0;
    for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
        const int2 offset = int2(dx, dy) * int(params.step);
        const int2 q = p + offset;
        if (any(q < int2(0, 0)) || any(q >= int2(params.size))) continue;
        const float4 tap = guide[q];
        if (tap.x <= 0.0) continue;
        const float4 value = filterIn[q];
        const float depthTerm = abs(tap.x - z) / (kSigmaDepth * abs(dot(gradient, float2(offset))) + kDepthFloor * z);
        const float luminanceTerm = abs(luminance(value.rgb) - l) * luminanceScale;
        const float w = kernel[abs(dx)] * kernel[abs(dy)] * pow(saturate(dot(n, tap.yzw)), kSigmaNormal) * exp(-depthTerm - luminanceTerm);
        sum += value.rgb * w; weights += w; variance += w * w * varianceAt(q, value);
    }
    filterOut[p] = weights > 0.0 ? float4(sum / weights, variance / (weights * weights)) : float4(center.rgb, centerVariance);
}
