// Ground-truth ambient occlusion (Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion", the XeGTAO arrangement) on the scene
// targets: a horizon search per pixel (aoMain, at the preset's resolution), a depth-aware blur (denoiseMain) and the composite into the scene colour
// (compositeMain, full resolution; fades with the GX fog and skips pixels that no depth-writing draw covered).
#include "gbuffer.slang"
#include "view_space.slang"
struct Params {
    float4 viewport;     // the scene viewport in scene-target pixels: x, y, width, height
    float4 viewA;        // view parameters: xScale, yScale, xOffset, yOffset
    float2 viewB;        // depthA, depthB
    uint2 region;        // scene region size in pixels
    uint2 aoSize;        // size of the occlusion images
    float radius;        // view-space radius of the search
    float falloff;       // fraction of the radius over which samples fade out
    uint slices;
    uint steps;
    uint noise;          // frame index for the noise pattern (0: a fixed pattern)
    uint flags;
    float strength;
    float2 depthRange;   // the viewport's near and far depth
};
[[vk::push_constant]] ConstantBuffer<Params> params;
[[vk::binding(0, 0)]] SamplerState linearSampler;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float4> sceneGBuffer;
[[vk::binding(3, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> sceneColor;
[[vk::binding(4, 0)]] Texture2D<float> rawOcclusion;
[[vk::binding(5, 0)]] Texture2D<float> occlusion;
[[vk::binding(6, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> rawOcclusionOut;
[[vk::binding(7, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> occlusionOut;

static const float kPi = 3.14159265;
ViewParams viewParams() { ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y; return v; }
float projectionDepth(float stored) { return (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x); }
// View position of a full-resolution pixel (centre) and whether it is a surface of the scene.
bool surfaceAt(int2 pixel, out float3 position, out float3 normal) {
    position = float3(0.0, 0.0, -1.0); normal = float3(0.0, 0.0, 1.0);
    if (any(pixel < int2(0, 0)) || any(pixel >= int2(params.region))) return false;
    float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    if ((uint(round(texel.w * 65535.0)) & kGBufferHasNormal) == 0) return false;
    float stored = sceneDepth.Load(int3(pixel, 0));
    if (stored >= 1.0) return false;
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
    position = reconstructView(viewParams(), ndc, projectionDepth(stored));
    normal = decodeGBufferNormal(texel.xy);
    return true;
}
float3 viewAt(float2 pixelCentre) {   // position of a (possibly off-surface) sample: depth only
    int2 pixel = int2(floor(pixelCentre));
    pixel = clamp(pixel, int2(0, 0), int2(params.region) - 1);
    float stored = sceneDepth.Load(int3(pixel, 0));
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;   // the position of the pixel centre the depth belongs to
    return reconstructView(viewParams(), ndc, projectionDepth(min(stored, 0.99999)));
}
// The cosine-weighted visible arc of one slice side up to horizon angle h (GTAO paper eq. 10, per side), for a normal at signed angle n.
float gtaoArc(float h, float n, float cosN) { return 0.25 * (-cos(2.0 * h - n) + cosN + 2.0 * h * sin(n)); }
float interleavedGradientNoise(float2 pixel, uint frame) {
    pixel += 5.588238 * float(frame);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("compute")] [numthreads(8, 8, 1)]
void aoMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.aoSize)) return;
    float scale = float(params.region.x) / float(params.aoSize.x);
    int2 pixel = int2((float2(id.xy) + 0.5) * scale);
    float3 position, normal;
    if (!surfaceAt(pixel, position, normal)) { rawOcclusionOut[id.xy] = 1.0; return; }
    float3 view = normalize(-position);
    if (dot(normal, view) < 0.0) normal = -normal;   // two-sided: back faces and flipped normals still occlude from the viewer's side
    float2 centre = float2(pixel) + 0.5;
    float radiusPixels = params.radius * params.viewA.x / -position.z * params.viewport.z * 0.5;
    if (radiusPixels < 1.5) { rawOcclusionOut[id.xy] = 1.0; return; }
    radiusPixels = min(radiusPixels, 0.25 * params.viewport.w);
    float noiseA = interleavedGradientNoise(float2(pixel), params.noise);
    float noiseB = interleavedGradientNoise(float2(pixel) + 17.0, params.noise);
    float visibility = 0.0, unoccluded = 0.0;
    for (uint slice = 0; slice < params.slices; ++slice) {
        float phi = (float(slice) + noiseA) / float(params.slices) * kPi;
        float2 direction = float2(cos(phi), sin(phi));
        // The same screen direction in view space: a point of the screen line, put at the pixel's own view depth, spans the slice plane with the view vector
        // (the plane through the camera that holds the whole screen line); its part orthogonal to the view vector is the slice direction.
        float2 ndcAlong = ((centre + direction * 8.0) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
        float3 ray = float3((ndcAlong.x + params.viewA.z) * -position.z / params.viewA.x, (ndcAlong.y + params.viewA.w) * -position.z / params.viewA.y, position.z);
        float3 sliceDirection = normalize(ray - dot(ray, view) * view);
        float3 axis = normalize(cross(sliceDirection, view));
        float3 projected = normal - axis * dot(normal, axis);
        float projectedLength = length(projected);
        float cosN = clamp(dot(projected, view) / max(projectedLength, 1e-5), -1.0, 1.0);
        float n = sign(dot(sliceDirection, projected)) * acos(cosN);
        float lowHorizon0 = cos(n + kPi * 0.5), lowHorizon1 = cos(n - kPi * 0.5);   // the tangent plane: nothing occludes below it
        float horizon0 = lowHorizon0, horizon1 = lowHorizon1;
        for (uint step = 0; step < params.steps; ++step) {
            float s = (float(step) + noiseB) / float(params.steps);
            float2 offset = direction * max(s * s * radiusPixels, 1.0 + float(step));
            [unroll] for (int side = 0; side < 2; ++side) {   // side 0 walks along the slice direction (positive angles), side 1 against it
                float3 delta = viewAt(centre + (side == 0 ? offset : -offset)) - position;
                float distance = length(delta);
                float weight = saturate((params.radius - distance) / max(params.radius * params.falloff, 1e-5));
                float shc = dot(delta / max(distance, 1e-5), view);
                if (side == 0) horizon0 = max(horizon0, lerp(lowHorizon0, shc, weight));
                else horizon1 = max(horizon1, lerp(lowHorizon1, shc, weight));
            }
        }
        float h0 = -acos(clamp(horizon1, -1.0, 1.0)), h1 = acos(clamp(horizon0, -1.0, 1.0));
        h0 = n + clamp(h0 - n, -kPi * 0.5, kPi * 0.5);
        h1 = n + clamp(h1 - n, -kPi * 0.5, kPi * 0.5);
        visibility += projectedLength * (gtaoArc(h0, n, cosN) + gtaoArc(h1, n, cosN));
        unoccluded += projectedLength * (gtaoArc(n - kPi * 0.5, n, cosN) + gtaoArc(n + kPi * 0.5, n, cosN));   // the same slice with the horizons on the tangent plane
    }
    // Normalised by the unoccluded estimate of the same slices: a slice's weight depends on its orientation, so with a few slices the raw sum would
    // darken grazing surfaces; the ratio is exactly 1 on a flat surface whatever slices were drawn.
    rawOcclusionOut[id.xy] = pow(saturate(visibility / max(unoccluded, 1e-4)), 1.5);
}

[shader("compute")] [numthreads(8, 8, 1)]
void denoiseMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.aoSize)) return;
    float scale = float(params.region.x) / float(params.aoSize.x);
    float3 centrePosition, centreNormal;
    float centreOcclusion = rawOcclusion.Load(int3(id.xy, 0));
    if (!surfaceAt(int2((float2(id.xy) + 0.5) * scale), centrePosition, centreNormal)) { occlusionOut[id.xy] = centreOcclusion; return; }
    float sum = 0.0, total = 0.0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        int2 at = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(params.aoSize) - 1);
        float3 position, normal;
        float value = rawOcclusion.Load(int3(at, 0));
        if (!surfaceAt(int2((float2(at) + 0.5) * scale), position, normal)) continue;
        float weight = 1.0 / (1.0 + 50.0 * abs(position.z - centrePosition.z) / -centrePosition.z) * pow(saturate(dot(normal, centreNormal)), 4.0);
        sum += value * weight; total += weight;
    }
    occlusionOut[id.xy] = total > 0.0 ? sum / total : centreOcclusion;
}

[shader("compute")] [numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    float3 position, normal;
    if (!surfaceAt(int2(id.xy), position, normal)) return;
    float fog = sceneGBuffer.Load(int3(int2(id.xy), 0)).z;
    float scale = float(params.region.x) / float(params.aoSize.x);
    float2 aoPosition = (float2(id.xy) + 0.5) / scale - 0.5;
    int2 base = int2(floor(aoPosition));
    float2 fraction = aoPosition - float2(base);
    float sum = 0.0, total = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {   // bilinear taps, each weighted by how close its depth is to this pixel's
        int2 at = clamp(base + int2(i & 1, i >> 1), int2(0, 0), int2(params.aoSize) - 1);
        float3 tapPosition, tapNormal;
        float value = occlusion.Load(int3(at, 0));
        float bilinear = ((i & 1) != 0 ? fraction.x : 1.0 - fraction.x) * ((i >> 1) != 0 ? fraction.y : 1.0 - fraction.y);
        float depthWeight = 1.0;
        if (surfaceAt(int2((float2(at) + 0.5) * scale), tapPosition, tapNormal)) depthWeight = 1.0 / (0.01 + 50.0 * abs(tapPosition.z - position.z) / -position.z);
        sum += value * bilinear * depthWeight; total += bilinear * depthWeight;
    }
    float ao = total > 0.0 ? sum / total : 1.0;
    float4 color = sceneColor[id.xy];
    color.rgb *= lerp(1.0, ao, saturate(params.strength) * (1.0 - fog));
    sceneColor[id.xy] = color;
}
